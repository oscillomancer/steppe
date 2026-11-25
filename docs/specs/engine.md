# Spec: the engine

**Status:** Implemented; amended with each change (dated sections and ADRs).
**Decision records:** ADR-0004 (backends), ADR-0008 (model/plan
separation), ARCHITECTURE §3 (threads, channels, RT rules, main-thread
budget), §6 (bridging, latency changes).
**Code:** `host/engine` (`supple::engine`), with plugin hosting and the
bridge process in `host/clap-host`.

The engine turns immutable session versions into sound: in real time through
an audio backend, or offline as fast as possible. Its non-negotiables are
real-time safety, determinism, and REAPER-class efficiency (anticipative
processing, flexible isolation).

---

## 1. Compilation: Session version → RenderPlan

The model thread (or a worker it delegates to) compiles a version into a
`RenderPlan` whose graph and setup are immutable: a structural change means a
new plan, compiled complete from the session; instances, recycled rings and
state carry over from the previous one (§1.6).
Its DSP state belongs to the executors, and strip, send and VCA levels and
pans, FX wet and bypass, and mute and solo arrive through its mix slots
(ADR-0060, ADR-0093, §1.6) without a new plan, as patch-only FX edits reach the
instances (ADR-0092, §1.6).

### 1.1 Nodes

- **StripNode** per track *(amended 2026-09-30, ADR-0019)*, stages by strip type:
  channel `inputs → [tap: pre_fx] → items/takes → inserts → mono/polarity →
  [tap: pre_fader] → pan, fader, mute → C9 Channel → [tap: post_fader]`; buss
  `input sum → C9 Buss → [pre_fx] → inserts → mono/polarity → [pre_fader] →
  C9 Channel → [post_fader]`; master `input sum → C9 Buss → [pre_fx] →
  inserts → mono/polarity → [pre_fader] → plain gain/pan → its own items →
  [post_fader]` → outputs/renders/monitor chain (ADR-0057). Mono and polarity (ADR-0022) act on the
  main pair, ramped like mute, never between an encode and a decode. With
  `project.console9` off the C9 stages run linear (same gains and pan laws,
  no curves, noise or dither; ADR-0019 §6). *(Amended 2026-11-10, ADR-0066:)*
  a channel's or buss's pan is a 2×2 matrix from its pan mode (`pan`,
  `dual`, `width`), positions and law (`pan.h`), applied with the fader
  before its C9 Channel; the four coefficients ramp with the fader, or follow
  automation per sample. The C9 stages run in `Staging::Unity` (no pan stage,
  no encode trim, ×φ after the decode), so a hop is unity for small signals
  and only each strip's own law cuts the centre. Following sends, key routes
  and rack inputs use their source strip's matrix; a positioned send uses
  `pan` mode under the source's law. Take pan is a balance (the 0 dB law).
  `width`, `pan_left` and `pan_right` are mix values (ADR-0060) beside `pan`. A channel's items play *before*
  the chain (as in REAPER); input monitoring and record inputs enter channels
  at `inputs`. A master's items (audio only, its prints) play after its
  plain stage, at timeline shift `S + chain`, into a buffer of their own that
  joins the output there: no decode, inserts, fader or pan act on them; the
  master's mute ramp (mute, its envelope, VCAs, solo silencing) does, and
  `lane_mute` leaves them out of the plan. Take FX and envelopes run as on a
  channel. The node keeps its output before them (`mix`) for a recording.
  The plan checker requires channels' items before the chain, masters' after
  the plain stage, and none on busses.
- **SendEncoder** per route into a buss/master that cannot share the
  source's `post_fader` tap: the route's polarity/mono, then its own C9
  Channel (fader = send level, plus the strip's fader for post-fader
  sends; pan = send pan or the strip's), then the edge. Edges into a buss
  or master only sum, channel-map and delay (ADR-0019 §3); a plan-level
  checker asserts it in debug builds and in the strip fuzz test.
- **Rack sends** *(ADR-0075)*: a route from a rack's Out module into a buss
  or master. Its level and mute (with the 10 ms ramps, its envelopes, and
  its home channel's mute, mute curve, VCA mutes and solo silencing) run
  at the rack node after the Out, linear, never Console 9 (`Edge::rackSend`,
  `encoded`); the destination adds that signal to its main pair right after
  its C9 Buss decode, before auxiliary links and the pre-FX tap, through
  its compensation delay. The checker requires a rack send's destination to
  decode and an Out edge into a channel not to. A rack send's edge belongs
  to the rack's executor (`syncMix` reads the home's mute from its slot),
  and so do its automation cursors: a loop wrap or locate resets them
  through the rack node's `out` edges, never the destination's `in` (the
  destination may be live while the rack renders ahead); across the
  frontier the ring carries its output.
- **ItemPlayer** (inside a TrackNode): per active item on the track's lanes,
  per take: source reader (audio: disk-streamed decoded+resampled blocks
  from the media cache; MIDI: event generator from the source with playrate
  and loop), take FX chain, take envelopes, item gain/fades/crossfades. As
  built, a take's envelopes are `volume`, `pan` and `mute`. An envelope with
  `param: "pitch"` can only come from a file (`env.add` refuses it with
  `env.param`): the plan has no slot for it, so it is never played, on an item
  or a clip, and nothing warns. The take's own `pitch` (a constant, in
  semitones) plays. Playing the envelope needs the stretch path to take a
  varying ratio (the lead's decision, with clips; ADR-0097).
- **FxInstance** wrappers around `host::Instance` (in-process) or a bridge
  proxy (§5), with wet/dry, bypass, pins, oversampling, parallel groups and
  containers compiled into a small per-chain program. As built: containers
  are not implemented. A stage with `parallel` runs beside the one before it
  on the same input and the branches' outputs are summed (each branch is
  what that plug-in would produce in series, so a bypassed branch adds its
  dry input); pinned stages and strips with auxiliary channels run in
  series, with a warning. `oversampling` 2/4/8 runs the plug-in at that
  multiple of the plan rate behind `dsp::HalfbandOversampler` half-band resamplers
  on its main pair and key (not with auxiliary outputs: those run at the
  plan rate, with a warning); event times scale by the factor.
- **RouteEdge**: copy from a source tap with gain/pan/channel map (and MIDI
  filter), plus its PDC delay line.
- **HwInput / HwOutput** nodes bound to backend ports.

Looping MIDI takes wrap `start_offset` modulo the source's beat length, as audio
loops do. Compilation covers the initial partial cycle, every full repetition,
and the final partial cycle; notes crossing the item's start are retriggered
there and their note-offs remain correctly placed. SMF export is limited to
16 MiB of encoded bytes, including long-delta padding; larger output returns
`midi.smf_too_large` before padding is emitted.

### 1.2 Buffers

All buffers are allocated at compile time: per node, `channels × maxBlock`
float per tap in use, one input-sum buffer, per-edge delay lines sized for
the edge's compensation, per-plugin scratch for pins/oversampling. MIDI/event
buffers are fixed-capacity arenas (overflow drops and counts).
Summation of a node's inputs happens **in a fixed order** (edges sorted by
source node order, then edge ID) after all inputs for the block exist —
never by concurrent accumulation — so results are thread-count independent.

### 1.3 Plugin delay compensation

The instance pool tracks raw plug-in latency at the activation rate separately
from prepared stage latency in plan frames. Service compares raw with raw,
so oversampling does not cause repeated reloads. Actual plug-in-originated
latency/restart requests still rebuild PDC. `Engine::rackLatencies` copies the
latest prepared rack's node and output timing on the main thread without
instantiating plug-ins (ADR-0064). It matches the version recorded at plan
compilation, not the latest submitted session: playrate scheduling may defer
a structural edit while the preceding plan remains prepared.


- Each FX instance reports latency (CLAP `latency`); bypassed instances keep
  their latency (compensated bypass); offline instances report 0.
- A chain's latency: sum over serial stages, max over a parallel group
  (shorter branches delayed to match inside the chain program).
- **Read-ahead (ADR-0051):** retain the forward arrival `A(n)` and the
  main output's startup shift `L = A(main) + chain(main)`. Solve backwards
  from main for each node's downstream latency `D` and timeline shift
  `S = L - D`. Known timeline material is evaluated at processing position
  minus `S`: it runs `D` samples ahead of the audible main-output position.
  For an edge from `u` to `v`, source tap offset `a` and destination offset
  `b`, `S(u) = min(S(v) + b - a)` and
  `delay = S(v) + b - S(u) - a`. Pre-FX taps have offset zero, other main
  taps and auxiliary source channels use the source chain latency. Key
  destinations use `stage.before`; main sums and auxiliary destinations use
  zero. Each auxiliary link participates separately. Padding remains on
  shorter fan-out paths; independent sources arrive at a sum aligned without
  padding the faster source. Parallel FX retain their internal alignment.
- The same main-master anchor is used in realtime and offline plans. Nodes
  not upstream of main keep a forward schedule against already solved source
  shifts, no earlier than their forward arrival. Alternate master/stem renders
  trim at the selected tap's own `S + tapOffset`, after preparation. Main
  `output_latency` retains its startup/playhead meaning.
- Clips, take output spans/fades/envelopes and track MIDI use `S`. A take's
  input uses `S - take.latency`. A take whose input runs ahead (`S -
  take.latency < 0`) takes, after a locate that restarts it (play, a seek, a
  strip started afresh; not a loop wrap or a continuing handoff), its notes
  from the locate on: those inside its look-ahead are due already and play
  at once, late, at the first frame, on the unity and the playrate path
  alike, as an offline render from there plays them. Looping, such a take
  reaches the loop end its look-ahead early: there its held notes end and it
  takes the loop start's notes and audio, so they sound on time after the
  wrap, every pass, on both paths (the playrate path through its timeline
  phase, the unity path given the executor's loop). The take runs wherever
  its input, wrapped or not, meets its item, so this holds for an item that
  ends before the loop end or starts at the loop start. A note at the loop
  start is taken; one at the loop end is not. A note crossing the loop end
  ends at the wrap. At unity it needs a look-ahead shorter than the loop: a
  loop no longer than it has no early wrap, and on every pass after the
  first the notes inside the look-ahead after the loop start are lost; so is
  its audio where the item ends at the loop end, and where the item goes on,
  what follows the loop end plays in its place (STATUS.md). Between the early
  wrap and the output's: a seek is a locate; a plan swap that keeps the
  chain's latency and its item's shift continues the take, and one that
  changes either wraps it afresh in the new chain's coordinates; a loop edit
  (another loop, or none) is a locate for the take, ending the notes it took
  early (the anticipative engine restarts its render for any loop edit
  anyway). Notes its block has no room for are counted in `voiceDropped` on
  both paths. Track/take stage parameters, output-event timestamps and plugin
  transport use that input shift plus `stage.before`, at unity rate as well as
  under playrate. Fader controls use `S + chain`; send encoders use their
  source tap's shift, and unencoded key controls use their destination
  stage's. Device audio/MIDI are never read ahead.
- The solve is independent of live/arm/monitor classification. Frontier rings
  retain matching monotonic stream-frame coordinates between executors; source
  read-ahead is relative to the retained startup shift, not an additional
  request into a future ring position. Latency exceeding the 200 ms renderer
  horizon therefore needs no renderer-lead fallback. All preparation/storage
  allocation remains off the audio path.
- Timeline material stays sample-aligned, renders deterministic, and realtime
  output bit-identical to offline output of the same plan in both executors.
  Old forward-padding render bytes are not preserved: Console noise/dither,
  per-frame state, parameter-grid phase and corrected unity-rate plugin
  transport may change (ADR-0051).
- `fx.pdc = ignore` (set in the project file; no command or GUI control sets
  it) excludes an instance from compensation: its own wet/dry and pin delays
  still match it, but its chain reports none of its latency, so its track
  arrives late by that much. The plug-in still delays its own audio, so a
  monitored input through it is not heard any sooner; only padding added
  elsewhere to align with it goes away.
- An oversampled stage's latency is the resamplers' round trip (72/80/83
  samples at 2/4/8x; an instrument's output crosses only the downward half)
  plus the plug-in's own latency at the high rate, padded at the high rate to
  a whole number of plan samples, so compensation stays exact.
- Input monitoring incurs only its own downstream compensated path latency
  (including fan-out alignment), input FX, ignored FX's physical latency and
  device latency. Unrelated playback latency adds no padding at a common sum.
  Recording starts at callback play position minus the plan's output latency
  and the backend's output plus input latency; ordinary monitored track/bus/
  master latency is not subtracted separately. Input-FX capture is corrected
  separately as below.
- Input FX (a channel's `input_chain`) process its record input whenever
  the channel is armed with an audio input, monitored or not: such a channel
  is live (the device input exists only in the callback). Monitoring hears
  the processed input, and the recorder records it, dropping the input
  chain's latency from the file's start so the take lines up with what was
  played (the file ends with that many frames of silence). Input FX are
  outside compensation and play no part in offline renders.
- Runtime latency changes follow ARCHITECTURE §6: `request_restart` → new
  plan built in the background → swap at a block boundary with a 10 ms
  crossfade on affected paths. As built (2026-10-06): a restart the plug-in
  asks for while processing (CLAP `request_restart`, VST3 `kLatencyChanged`,
  e.g. a "Low Latency" mode) is `Instance::restartPending()`; the pool marks it
  `restarting`, the engine reloads, and that plan leaves the instance off both
  executors' instance lists (its stage passes through, compensated). Once the
  executors have adopted it the instance is no longer processing, the next
  `InstancePool::service()` restarts the same instance (its state kept), and a
  latency that moved since the last service (`latencyChanged`) reloads the plan
  with the new compensation.

### 1.4 Automation

Envelope points are pre-converted to plan time (samples at the engine rate,
per the time map; beat-timebase points are converted through the tempo map
at compile time). Per block, each automated target evaluates its segments:
- mixer targets (volume, pan, width, pan_left, pan_right, pan_boost, mute,
  send level/pan) interpolate per sample
  (gain ramps) and drive the C9 Channel (or the plain stage) — cheap and
  exact; Console 9 stages restart their seeded dither at every locate.
  Channels and busses bind the three new percentages and compensation into
  the strip's `PanSettings` per sample; compensation ≥0.5 overrides the
  effective law's boost while its envelope is active and nonempty. PanCache
  recomputes only on a setting change. Following sends, keys and rack inputs
  use private copies with their own cursors, timeline shifts and write holds;
  positioned sends also inherit automated compensation under the source law;
- plugin parameters receive CLAP param-value events at each envelope point
  and at every point of the absolute 32-sample timeline grid
  (`kAutomationStep`) inside curved/linear segments, when the value changed
  since the last one sent, in the parameter's CLAP domain (normalized for
  stock plugins, ADR-0010). The step is fixed; there is no setting for it;
- write/touch/latch modes record from GUI/control input into the model on
  the main thread (reliable coalescing channel, ARCHITECTURE §3), creating
  one version per gesture. Capture timestamps are seconds; evaluation, entry
  guards, erase windows and stop holds convert them into each envelope's
  stored domain (seconds for `time`, tempo-map beats otherwise). Plugin-output
  recording follows the same conversion, and replay carries time-envelope
  points as explicit seconds through `env.points.set`.
- write holds (ADR-0061): once a pass has written a strip's or send's
  volume or pan (including width, dual positions and compensation on channels
  and busses), or a VCA's volume, the engine plays that target's static
  value (the hand) instead of its compiled curve until the hold ends, and
  the points the pass keeps writing reach the model without compiling
  (§1.6). Latch, latch_preview and write hold until the pass ends at pause
  or stop; touch until 500 ms after its last write (`EngineApp`'s service
  clock: nothing reports a strip control's release). Ending a hold compiles
  the session, so the written curve plays from there; a held value ramps over
  the smoothing time like a static one. Any load carries hold changes,
  including the playrate scalar's shortcut, and a released stale curve always
  compiles. Holds exist only while the transport plays; undo, redo and a
  branch switch mid-pass start the pass again at that point, so the
  abandoned writes neither stay held nor get a latch tail at stop. A halt
  ends the pass that was playing. Mute and the master playrate are not
  held: their writes compile as before.
- plug-in parameter holds (ADR-0061, amendment 2026-11-20): a held lane
  sends nothing, so the plug-in keeps what the hand set. Two things hold it:
  - **the executor** (`ParamLane::gesture`, `latched`), on a real-time plan
    (`Plan::gestureHolds`) for a lane whose envelope is in a writing mode.
    It holds from the block after the plug-in reports `GestureBegin` while
    playing (`holdGestures`, after each process call). Touch holds to the
    block with `GestureEnd`; a latching mode holds on until a start or seek
    (`locate` with a console restart) or a stopped block. A plan swap
    carries an open gesture, and a latch whose main-thread hold has not
    fallen;
  - **`EngineApp`'s hold** (`ParamLane::held`, a `MixSlots::lanes` flag per
    lane by envelope), from the touched envelopes as for a level. When it
    falls, the executor's latch goes too.

  Released, the lane sends its value at once (`lastSent` is cleared while
  held), then on its grid. The first block of a gesture is not held. Points
  written into a plug-in envelope compile, since they change the FX's
  values too.

### 1.4.1 Modulation (ADR-0096)

A modulation route (`mr_*`, session-model "Modulation") moves a target
parameter by a rack's Mod Out signal × its `depth`, as an offset on top of
the base: the static value, the envelope or a write hold's hand. It never
writes the base, the model or an envelope, and write modes record the base
only. A held plug-in lane (§1.4) carries no modulation through its envelope
until it is released.

- **Edges.** Every route, enabled or not, is an edge from its source rack's
  node to the node that applies its target; `enabled` only zeroes its
  contribution (ADR-0096, amendment M-l). That is the track for an FX in a chain
  and for a track's mixer targets; for a send's level it is the node that
  applies the level (the source strip, or the rack for a rack send). The
  edge orders the source first in `topoOrder`, is a dependency in both
  executors' schedules, and joins the latency solve (§1.3) as a sidechain
  into the target stage:
  - its tap latency is the Mod Out's latency inside the rack (computed as
    an Out's, rack.md §6);
  - its `before` is the chain latency ahead of the target stage: 0 for an
    input-chain stage, which is outside compensation; for a mixer target
    the whole chain less one grid interval (32 frames), since it reads one
    interval ahead;
  - it carries a delay line of held values sized by that compensation, so
    the value the rack computed for timeline sample t reaches the target as
    the target plays t;
  - under a master playrate the delay, the rack grid Mod Out samples on and
    the target's 32-frame grid are in device frames, the graph's domain
    (ADR-0045), as a sidechain's delay and the `Param` grid are.
  A late source path delays the target as a sidechain does. Cycles are
  refused at edit time (`modulation.cycle`); self-modulation is not
  supported.
- **Liveness.** A modulation edge propagates liveness both ways, like a MIDI
  edge (§2.1): a live source makes its target live, and a live target pulls
  its source live. A route's source and target therefore always share an
  executor and nothing crosses the frontier rings. A route that moves a
  node into the live region adds a `modulation.live` plan warning.
- **Plug-in targets** (CLAP parameters flagged modulatable). At each point
  of the target stage's absolute 32-sample automation grid, the stage sends
  one `PARAM_MOD` event per modulated parameter, after any `Param` event at
  the same frame: the sum of its routes' signal × depth × the parameter's
  CLAP range, a disabled route adding 0. An event is sent only when the
  value differs from the memo of the last one sent.
  - The memo is carried across compiles by (fx, param), like `carryMix`
    (§1.6): a stage's slots are matched by instance and CLAP id. A
    parameter that the new plan no longer modulates is sent one 0 at its
    first grid point and leaves the memo. So that a carried memo always
    fits, each stage keeps a slot for every parameter any plan has
    modulated on its FX (`InstancePool::modulatedParams`, main thread), and
    a stage with no route left keeps its slots until their 0s are sent. The
    memo follows its instance between the track's input chain and main
    chain. It is not carried to another node: an FX moved to another track
    starts with every slot stale, so the first grid point resends what is
    modulated there, but a parameter the new plan no longer modulates (a
    route removed in the same edit as the move) keeps its last offset until
    the instance's next epoch (activation, reset, patch or state).
  - It goes stale (it is never emptied) on the instance's activation, a
    reset (a seek, a play start), a changed instance, and a patch or state
    applied to the instance: `host::Instance::modulationEpoch()` counts
    `activate()`, `reset()`, `setPatch()` and `loadState()` (`resets()`
    keeps counting `reset()` alone). The next grid point then sends the
    current sum of every modulated parameter and 0 to every other parameter
    the memo lists, and drops those. A stage that does not process this
    block (a compensated bypass, not owned, not processing) is sent nothing
    and its memo goes stale.
  - The route's delay runs every block at the target node, and a locate
    clears it unless a swap just carried it (by route id, at the same
    length), as an edge's delay.
  - A patch edit to a modulated FX compiles instead of taking the patch path
    (ADR-0092).
  - Each stage reserves `block / 32 + 2` events per modulated parameter and
    one for each other slot. An event that finds the block full, or a
    bridged block at the bridge's cut (`host::Instance::kBridgeBlockEvents`,
    2048), waits for the next grid point, which sends the then-current sum.
    Each wait is counted (`Stage::modDeferred`): an offline render warns per
    FX, and the real-time engine adds the count to
    `Engine::modulationDeferred()` (`engine.status` `modulation_deferred`)
    and warns once per rise (`modulation.events`). Input-chain stages reserve their
    automation and modulation events as chain stages do.
  - Bridged plug-ins receive the same events through the bridge; a late
    block's coalescing keeps the latest value per target (§5).
- **Through an envelope (M4).** A VST3 parameter, or a CLAP one without
  the modulatable flag, that is automatable and neither stepped nor an enum
  binds to its automation lane (`ModEdge::viaLane`, `ParamLane::modFirst`
  and `modCount`, `Stage::laneModRoutes`). The lane sends
  `clamp(base + Σ depth × signal × range, min, max)` at its usual emission
  points: its first frame, each point, and each 32-sample grid point while
  a route is bound. The base is the envelope's value at that frame. A
  VST3's lane is in the normalized domain (range 1); a CLAP one's in plain
  values (range max − min). While the source rack is owned by the other
  executor, the last sum holds. The lane has a telemetry slot of its own,
  stored as a fraction of its range. Without an active envelope on the
  parameter, the route is inert (`modulation.target`, "has no active
  envelope to carry it"). In a write pass the lane is held from the
  plug-in's gesture (§1.4), so what the plug-in reports is the hand's value,
  recorded as it is. Outside a held pass (stopped, or a reading mode) the
  route's current offset is taken off what the plug-in reports
  (`commitOutputGesture`), so the static value keeps the base. What a
  plug-in merely repeats of the values it was sent never gets that far:
  each lane notes the stage block in which it last sent its parameter, and
  the executor drops, before the main thread, a global output value for a
  parameter its lane sent in that block or the one before, outside the
  plug-in's own gesture on it, whatever the value (review round 81, E1 and
  E2: matched by parameter, so no count of lanes or points overflows it,
  an echo one block late is caught, and so is a stepped plug-in's report of
  the step it plays). That holds for every format, bridged or not, and for
  plain automation as for this lane. What it gives up: a gestureless edit
  of a parameter while its lane sends, which the lane overwrites within 32
  frames anyway. A VST3 editor's `beginEdit`/`performEdit`/`endEdit`
  reach the executor as the plug-in's gesture (a `performEdit` outside
  `beginEdit` is one of its own), after what the processor reports in that
  block, so they are recorded and hold a writing lane as a CLAP gesture
  does; what the processor reports in that block for a parameter the
  editor moved is that edit repeated and is not passed on (review round
  81b, E5). A barrier that drops queued edits, a deactivation or a full
  queue ends every editor gesture still open, so a lost `endEdit` leaves
  no gesture open in the stage. The ends go out in queue order: after
  every edit queued before them, and before any queued after, each request
  in its own place (review rounds 97 and 98). A block takes the editor's
  queued edits only as far as its 64 editor events can hold them with the
  ends that may follow; the rest waits for the next block, which it asks
  for, so a drag made while the plug-in was not processing comes out whole
  (round 98, U1). Closing the editor with a grab open (no
  `endEdit`, review round 81c, E6) queues each grab's end behind the
  editor's moves, so a move made just before the close stays inside its
  gesture: one step, its points once (round 92b; ending it ahead of the
  queue made the move a second, gestureless edit). A processing VST3's
  editor edit does not flag the plug-in's state as changed: the gesture's
  commit saves the state, and a capture of its own was a step that swapped
  the plan, which dropped the rendered gesture before it was heard (the
  anticipative executor; review round 81, E2). It asks the stage to process
  instead, so a bypassed stage still reports it. Only an instance that is
  not processing flags its state,
  and so does one that stops processing with edits still queued. Outputs a
  plan swap drops (pending, or drained under an older plan) that carry an
  edit capture their plug-in's state, so the edit still reaches the
  history, as a state step. Without the filter an echoing plug-in under
  modulation added a history step per echo, stopped or playing, and wrote
  points in a writing mode (the gui-lead's second walk, defect 1).
- **Inert routes.** A route is inert, with a plan warning, when binding finds
  that its target cannot take it: the parameter is gone, read-only, bypass,
  neither modulatable nor reachable through an active envelope, or the
  plug-in is missing (`modulation.target`), or for a mixer target when its node applies no
  such level (a send that is not an encoded edge of its source). A route
  into a frozen track is inert silently: the print plays. It is also inert
  after its target reported a
  latency change while modulated (`modulation.latency`): the engine then
  keeps every route into that FX's modulated parameters disabled until one
  of them is edited, the FX is replaced, or the project is reopened.
- **Telemetry (M2).** Prepare gives each bound target a slot in
  `Plan::modOffsets` and each route its target's (`modOffsetOf`). The
  executor that runs a target stores its offset with a relaxed store: a
  plug-in's sum at every grid point, sent or not, as a fraction of its CLAP
  range; a mixer target's last frame of each block, in dB or percent.
  `Engine::modulationOffsets()` (main thread) reads the newest plan's, by
  route; `modulation.telemetry` adds each route's span (depth × its signal
  range).
- **Rack handover.** While a rack's modules are still owned by the other
  executor (rack.md §6, "State"), its Mod Outs hold their last values and
  the emitters they feed send nothing for that block.
- **Depth and `enabled`** are values published without compiling (M-h): one
  mix slot per route (`MixSlots::modulations`), copied at block start by the
  executor of the route's target, which runs its source too (§1.6,
  ADR-0093). A route's source, target and `lane` compile. An edit to a route
  into an FX whose routes a latency change turned off compiles, since that
  edit turns them back on.
- **Mixer targets** (M-i: track volume, pan, width; the level of an audio
  send between strips or a rack send). Prepare binds each to its node's
  `mixMods` (`Node::modVolume`, `modPan`, `modWidth`, `Edge::modVolume` on
  the node that applies the send's level: its source, or the rack for a rack
  send); routes on one target sum. At each point of the absolute 32-frame
  grid the interval's end becomes the sum of the route values read there,
  which the one-interval lookahead above delivers early, and the offset at
  each frame is linear between the interval's start and end. A rack the
  other executor still owns holds the end. The interpolation is carried by
  target across plan swaps and starts at 0 after a locate, with the delays.
  The base (automation, else the fader's value) plus the offset is clamped
  per sample to the target's range (volume up to 12 dB, a level at −inf
  stays −inf; pan and width −100..100), before VCAs and the gain ramps; a
  modulated level is computed per sample. A modulated send is encoded.
  Volume and send level offsets are in dB, pan and width in percent.

Renders stay deterministic and real time equals offline at any block size,
in both executors and through the bridge: every grid point and delay is a
function of the stage's absolute position, a reset makes the memo stale so
the first grid point after it sends every modulated parameter, and racks
restart their modules at every locate (rack.md §6).

### 1.4.2 Clip launcher (ADR-0097)

*The lead's revision with L2, 2026-11-19; the ADR's "Playing" section says
the same.*

- **A track either follows the arrangement or plays one clip.** Every
  clip of a channel is compiled into the channel's node as a *clip
  player*. An audio clip's player holds the take's prepared buffer, as an
  item's does. A MIDI clip's holds its notes, sorted by their beat within
  the content. A launch never compiles. A frozen track's node compiles no
  players: it has no chain or note stage and plays its print.
- **What a track plays is transport state,** the node's *launch state*:
  `arrangement`, `clip` (with the clip's player and anchor), `held` (a
  `once` clip past its length) or `stopped`. Plans carry it across swaps
  by node id and clip id (§1.6 of engine.md). A clip the new plan no
  longer has (removed, or now past the memory budget) ends at that swap
  and its track returns to the arrangement. An audio clip without a take
  chain crossfades out over the switch fade: before publishing a plan,
  `Engine::load` copies into each node the previous plan's audio players
  whose clip it lacks (`Node::retiring`, `addRetiring`; main thread, never
  launched or listed), and the carry fades from that copy
  (`LaunchState::fadeFrom` −3), starting at the first block the node
  plays after adoption. A clip with a take chain stops at the swap (its
  chain's instances leave with the plan), and a MIDI clip's notes end.
- **A launch is a timed message (L-a, B1).** Its target is `{epoch,
  stream sample}`, the coordinate plan handoffs use (`handAt`,
  `handEpoch`). A scene launch has one target for all its tracks: the
  latest of their floors (B5).
- **The floor is per track.** It is the position of the executor that
  runs the track plus that executor's block: the renderer's published
  position plus one render block for a rendered track, the callback's
  stream plus one device block for a live one. The target is the next
  quantize boundary at or after the floor, through the playrate
  timelines.
- **The queue is the engine's (L-b).** It has fixed capacity and holds one
  entry per track; a newer launch on a track replaces its pending entry.
  *(An entry applied early, a clip chain's lead or an audio clip's warm-up
  under a pitch-keeping rate, is no longer pending and is not replaced;
  for a warm-up a later launch lands after its switch, §1.4.2's *Under a
  playrate that keeps the pitch*.)* Plans hold only the players, so an
  entry survives plan swaps and a plan waiting for adoption.
  - **Who applies it:** the executor that runs the track's node when its
    block reaches the target. It splits the block at the target, as at a
    loop wrap, and takes the entry by compare-and-swap, so a node handed
    between executors is applied exactly once. A handoff carries the
    applied state with the node.
  - An executor whose block already starts past the target marks the
    entry missed, and the main thread re-quantizes it. With a bar or beat
    quantize and a clip length in whole bars, a re-quantized track keeps
    the same phase (below).
  - **Epochs:** an executor never takes or misses an entry from another
    epoch. After a seek's `epochDone_` the main thread re-quantizes every
    pending entry from the new position (B4).
  - Each applied entry comes back to the main thread with its stream
    sample, its running frame and its song position.
- **While stopped,** a launch is not quantized. It starts the transport
  and applies at the play start's first sample, so the clip's anchor is
  the play position. A clip chain's early half would fall before that
  sample; it applies there too, live as in the replay, and the chain
  warms up through its first frames (its lead), as when a swap raises the
  lead (ADR-0097, amended 2026-11-20; review round 69).
  Under a master playrate the play start is a seek to the play position,
  which begins a new transport epoch: `play()` places the launches made
  while stopped again in that epoch, at its first sample, before the
  transport runs, so they still start at the play start and not on the
  next boundary (the builder's song journey 2, D1).
- **Quantize** is project-wide (`launch_quantize`) with a per-clip
  override. `none` means the floor: about the render-ahead (200 ms) for a
  rendered track, one device block for a live one. That is accepted for
  this work and stated in the guide.
- **Phase is a pure function of the song position (L-c),** in the clip's
  unit, with the mathematical modulus for negative differences.
  - MIDI: the position in the clip is `(song beat − anchor beat) mod
    length`. The player converts beats to samples per block through the
    plan's time map, without allocating, as timeline MIDI does.
  - Audio: it is `(timeline sample − anchor sample) mod length in frames`,
    never beats, so a tempo ramp does not move it.
  - Inside the period, a loop window that is on maps the position into
    the content, as an item's looped source does.
  - A `once` clip sounds while its position is within its length after
    the anchor. It is silent before the anchor, and silent after a
    transport wrap until it is launched again.
  - A loop clip plays nothing before its anchor until a wrap or a seek
    takes the song there; from then on it plays by its phase wherever the
    song is. A wrap or a seek also ends a switch's crossfade.
  - Seeks and wraps use the same formulas, so clips stay on the grid and
    a performance replays exactly from its log.
- **Switching (L-d, B5).** Leaving the arrangement or a clip ends what
  was left's sounding notes at the switch sample. An audio clip laid one
  item per period (`clipLaysAsOneItem` false) that fades out reads on in
  the pass it was in from the switch's song sample (`heldPass`), as its
  committed piece does, rather than wrapping inside the fade-out. Returning to the
  arrangement does not chase notes (`trackEvents`, engine.md §1.1). An
  audio switch crossfades over a fixed 5 ms of timeline samples, in the
  source like an item's fade, in both executors and offline; under a
  master playrate too, so it is heard as 5 ms divided by the rate.
- **The launch log and the offline replay.** The engine logs each applied
  launch with its running frame (stream frames since the play start, in
  the epoch's terms), its song position, the scene when `launch.scene`
  launched it, and its song frame: the boundary's sample on the song's
  unwrapped running count (`LaunchLogEntry::songFrame`; at unity the
  epoch's start plus the running frame, under a master playrate the rate
  timeline's unwrapped phase, computed at placement). The scene and the
  song frame are carried through the slot and the executor's ack; the
  replay ignores both. A transport wrap is not
  logged: it follows from the loop, which a replay is given. A seek, a
  loop change or a stop ends a capture, so a replay never crosses one
  (ADR-0097, revision 2026-11-20, batch 3).
  Renders and the render dialog play the arrangement only, and the dialog
  says so in one line while any track is held by a clip, following
  launches while it is open. An internal offline replay applies a log at
  its logged running frames; the tests use it.
- **A capture is kept (ADR-0097, revision 2026-11-24).** The engine holds
  the capture it runs; `EngineApp` keeps the last one (`keptCapture_`,
  `EngineApp::launchCapture`) across what destroys the engine: an engine
  stop (`stopEngine`), an unrequested device restart and a device failure
  the restart gives up on (both `beginRestart` → `haltEngine`). A capture
  running then is ended at the heard position first, as
  `launch.capture {on: false}` ends it (`CaptureView::endedBy`: the stop
  or the device). The kept one is what `launch.commit` and `launch.state`
  read until a newer capture has begun, heard a frame (one turned
  off before that replaces nothing); `launch.capture {on: true}` warns
  when it will replace an uncommitted one, naming its span, launches and
  version (`LaunchCapture::version`, the session version as it began). It
  stays committable with the engine stopped or started again, at any
  device rate: launch.commit converts by its own `sample_rate`.
  `engine.stop`'s answer, and the `engine.restarting` event or the
  `engine.error` that gives up (either way), carry a warning naming an
  uncommitted capture, said by what ended it; a client in-process reads the
  same lines from `EngineApp::captureKeptWarning` and
  `captureReplaceWarning`. A capture counts as committed while the head
  reaches the version launch.commit wrote it in
  (`LaunchCapture::committedIn`): by first parents, or through a merge of a
  branch whose line holds it (a merge that replays the commit counts as
  it). An undo of the commit makes it uncommitted again, a redo commits it,
  and an edit after the undo leaves it uncommitted (`captureUncommitted`).
  The mark is set only when the commit's transaction commits (a rolled-back
  script's launch.commit leaves none), and the app pins that version
  (`VersionStore::pin`), so a history squash or collection never loses it.
  `EngineApp::CaptureView` carries as fields all that the warnings say,
  for a client that words its own line: the span (`capture.startSample`
  and `frames`, and `startBeat`/`endBeat` on the current time map), the
  launches, `version`, `endedBy`, `kept`, `replacedOnBegin` (a newer
  capture is armed and replaces it once it hears a frame), and `commit`:
  `None` never written, `Committed`, or `Undone`, written and taken back,
  which the GUI reads as declined.
  `launch.state` says both at once: `capturing` for the engine's own
  capture, armed or running, and `capture_kept` for a kept one beside it;
  with the engine stopped it answers for a kept capture. The GUI's Capture
  commits whatever is uncommitted, the take just ended or a kept one (a
  take it committed, or whose commit was undone, is skipped), and a take
  the user drops is forgotten
  (`dropKeptCapture`). Replacing the project or quitting the app drops
  it: the GUI's unsaved-work dialog is to count it
  (`captureUncommitted`, not yet), and a headless app has no dialog.
- **Capturing a performance (L4, L-f).** While `launch.capture` is on, the
  engine keeps the song sample where the capture began, the transport
  loop it runs under, what each track that was not following the
  arrangement played then (its launch, anchor kept), and every launch
  applied after it, by running frame from the start
  (`Engine::launchCapture`). Its running time is song time: song frames
  from the start (stream frames at unity, the song samples played under a
  master playrate), heard through the executors' published rate clock
  (ADR-0097, revision "S3 step B"). It begins at the heard position (stopped:
  where playing starts) and ends at the heard position, or, ended with a
  quantize while playing (`launch.capture {on: false, quantize}`), on that
  quantize's next boundary: placed as a launch of every track with clips
  made then would be, so a scene launch made with it lands there too. One
  track's launch, with a nearer floor (a live track in the anticipative
  engine), can land a boundary earlier, and is then in the capture. The
  capture runs until that boundary is heard (`endsFrame`;
  `Engine::launchCaptureEnding` says one is pending) and then ends on
  it, on the sample a launch there switches on; a launch applied from the
  boundary on is not in it. `endsBeat` (`ends_at`) is that sample's beat,
  which the commit's `to` will be; after a transport loop wrap it is the
  song's beat in the loop, while `to` runs on by running time. On again before it cancels the end, another end replaces it, and
  `launch.commit` is refused meanwhile (`launch.capture.ending`). A
  relocation (a seek, a stop, a loop change) ends a capture that has run
  any frames, where it was heard, ending or not; one that has not begins
  again where the transport goes. A capture armed while a launched clip's
  MIDI note sounds commits that note struck again, at any rate: the
  committed piece starts at the capture's start, inside the note, and the
  arrangement strikes a note held into an item's start at that start, so
  the rest sounds from a new attack (a stated limit,
  test_clip_player_rate_armed.cpp); armed in a rest it is exact. A capture lives in one transport epoch
  (ADR-0097, revision 2026-11-22). It runs through master playrate edits;
  arming is refused with `launch.playrate` while a launched audio clip
  plays (one played once counts until the song heard is past its pass from
  its anchor, which a seek keeps, and no transport loop brings it back;
  round 117 W3) and the newest plan keeps
  the pitch, and such a plan arriving ends
  a running capture with a warning naming the clip (the stretch converter
  reading it has no committed equivalent). For the same reason a switch
  that leaves or returns to an arrangement with an audio item within
  reach of its boundary on the timeline (the converter's reach either
  side: 0.25 s of song, times the fastest master rate the plan plays
  where that is above 1, a sync chase's scale read from the compiled plan
  rather than the live chase, which can drift a few percent from it, well
  inside the reach's margin; under a transport loop the reach runs across
  the wrap, as the converter does; round 128 W1), queued
  while the newest plan keeps the pitch (`captureBeforeStretchCut`, at
  placement and at each service), ends the capture on that switch's
  boundary, the launch not in it, with a warning naming the track; MIDI
  launches on other tracks, and switches clear of a track's audio, are
  captured (round 117 W1). A launched audio clip's switch queued while the
  newest plan keeps the pitch ends it there as well, wherever the
  arrangement's audio lies, with a warning that it launches an audio clip
  (S3 step D: no committed item reads its converter's history). That end goes with its switch: replaced before
  its boundary (another launch on the track) or voided (an edit that leaves
  it nothing to play), it is dropped and the capture runs on; it is kept
  apart from a quantized end, the earlier of the two ending the capture,
  and on again cancels only the quantized one (round 117 W2).
- **Committing it.** `launch.commit` lays the capture out linearly: a
  launch at running frame f lands at the capture's start plus f, whatever
  the loop did. On each track that left the arrangement, the stretches
  where it played a clip, was stopped or held replace the arrangement:
  its items there are split, trimmed or removed (audio from 5 ms after a
  switch away, with a 5 ms fade out, and a 5 ms fade in where a clip
  returned it to the arrangement; MIDI at the switch itself). Each clip
  stretch becomes copies of the clip's takes, split where a loop wrap
  moved the song, each piece starting at the phase the song gave it (the
  take's start offset): one looped item when the clip's period is its
  content's whole pass and it has no fade, else one item per period. An audio piece starting
  at a switch from the arrangement or a clip fades in over 5 ms; one
  ending at a switch runs 5 ms on and fades out, the crossfade as items.
  The arrangement a loop repeated is not copied. What it writes plays as
  the capture was heard: exactly away from the switches, and to rounding
  through the fades.
  An audio clip with fades (S3 step C, Q4) is laid one item per pass,
  each with the clip's fade-in where it starts on a pass's start and its
  fade-out where it ends on a pass's end. A switch's fades give way to
  them as live: a piece starting on a pass's start with a fade-in takes no
  5 ms fade-in (rule 3); a switch whose 5 ms would reach the pass's
  fade-out runs the piece on to the pass's end under the clip's fade-out,
  with no 5 ms fade-out (rule 4); a switch exactly on a pass's end lays
  the next pass's first 5 ms, with the clip's fade-in and the switch's
  fade-out. A clip still playing at the capture's end inside its pass's
  fade-out played on live: its piece runs to the pass's end under its
  fade-out, past the capture's end, the arrangement under it cleared and
  fading in after it, and the reply's `to` covers it (review round 135).
  A loop wrap's refusal says how many samples (and ms) into the fade-out
  the wrap falls, so a rounding case shows as one. An item's fade runs from its own
  edge, so what a piece would cut out of a clip's fade is refused with
  `launch.commit.fade`, naming the clip, the track and the beat, the
  performance kept (rule 7): a piece starting inside the fade-in (the
  capture's start or a loop wrap there), one a loop wrap ends inside the
  fade-out, and a stand-in that a further switch cuts (rule 5) or a loop
  wrap crosses.
  A scene launched whole also writes a section. `launch.scene` logs each
  clip it starts with the scene's id, all on one running frame; the commit
  writes a section for a scene whose launches at a frame all still hold (a
  track's last at that frame: one taken over by a later launch of the track
  on the same frame spoils it). Clips launched one by one write none, however
  they line up. The section is a region (`section` true) named after the
  scene, in its colour, from where the items of that frame land (the
  launch's own beat before the loop's first wrap, the timeline's position of
  the running frame after it, as the items do) to the first later launch on
  any of the scene's tracks, else the capture's end.
  A section already there with the same name and span is kept (committing
  again writes the same performance over itself).
- **Memory (L-e).** The prepared-source budget counts sample values
  across channels: the decoded source plus each converted variant, with
  a source or variant shared by several takes counted once. A clip on
  audio the arrangement already plays at the same rate therefore costs
  nothing. The same preparation (`prepareSources`, off the audio thread)
  adds the clips' takes after the arrangement's, in scene order and then
  track order. It skips any clip that would pass 2^28 instead of failing,
  and names the skipped clips in a `launch.memory` warning. A launch of a
  skipped clip is refused with `launch.memory`.
- **Frozen tracks.** Freezing returns the track to the arrangement at the
  swap that adopts the frozen plan, in both executors, and drops its
  queued launch. An audio clip it played crossfades into the print as a
  removed one does into the arrangement (`retiringIntoPrint`, at the
  print's chain delay). `launch.clip` on a frozen track
  fails with `track.frozen`, and `launch.scene` skips it with a
  `track.frozen` warning.
- **Audio clips play at the take's own rate,** with its playrate, pitch
  and channel mode as on the timeline, and no warp. A clip whose audio
  does not fit the project tempo gets a `launch.tempo` warning.
- **Under a master playrate (ADR-0097, revision 2026-11-21).** Clips play
  on the playrate path (ADR-0045 §2), at the song positions the rate
  timeline gives:
  - *Placement.* The target stays a stream frame. `placeLaunch` seeds a
    copy of the running rate timeline from the executor's published clock
    (with the scalar edits after it and the envelope), finds the quantize
    boundary from the floor's song position, and targets the first frame
    whose unwrapped song position reaches it (`placeRateLaunch`). A rate
    control still on its way to the executors answers `launch.later`. A
    scalar edit voids the pending launches past its frame
    (`voidRateLaunches`), and a plan with other rate controls those after
    its handover; service places them again. A launch whose every slot was
    missed while it was placed is placed again at once (`launch()`, up to
    three tries).
  - *An audio clip's fades* (S3 step C, Q4 rule 1) shape every pass: the
    output frame's gain is an item's fades at k = its phase in the pass,
    on an item one period long (`clipFade`), after the reading: in
    `playerFrame` on the unity path, on the tape converter's output at the
    frame's song position on the playrate path (as `playRateClip` fades an
    item), and after a clip chain, as an item's fades follow its take
    chain. A pass held on past the period's end reads 0 under a fade-out.
  - *A clip's own fade stands in for a switch's* (Q4 rules 3 and 4): an
    audio clip with a fade-in comes in at weight 1, shaped by its fade-in
    from the switch, with no switch fade-in (`incomingStandsIn`); one
    switched away where the crossfade would reach its pass's fade-out
    (P − Lout − F < phase < P, not on the pass's end) plays on at weight 1
    under its fade-out to the pass's end and reads nothing after it
    (`outgoingEnd`, for the player, a clip chain's input and its weight).
    The region reaches F before the fade-out because a switch quantized to
    the fade-out's start lands a sample either side of it as the tempo
    rounds (review round 135, H1). What comes in fades in as usual or
    stands in. A clip with fades switched exactly on a pass's end has
    begun its next pass: it goes out over the crossfade in that pass, from
    its fade-in (`heldPass`). The stand-in's end is read from the playing
    plan: an edit of the clip's length or fade-out while it stands in
    (`clip.set`) moves it at once, and a shorter fade-out can end it at
    once, as a clip edit changes a playing clip.
  - *Audio* reads through a tape converter per player reading
    (`PlayerReader`): positions are the frames' unwrapped song positions,
    and its source folds them over the loop and maps them by the player's
    phase rule. It keeps no state between quanta, so a launch primes
    nothing. A clip that a commit lays one item per period
    (`clipLaysAsOneItem` false: its period is not its content's whole pass
    from the start, or it has a fade) reads each period alone, silent
    outside it, as that item does, so the converter does not read across a
    period's wrap; a
    quantum that crosses one is read again for its later frames
    (`PlayerReader::later`). A switch's outgoing side reads nothing past
    its crossfade, as the commit's pieces: the clip fading out (or a clip
    chain's input going out) is silent from the anchor plus 240 song
    samples, and the arrangement's tape items read nothing past it after a
    switch away, nor before the anchor while a switch back fades them in
    (`Clip::gateFrom`, set by `launchAudio`; a stretch converter is not
    cut). While a clip holds the track, its items' stretch converters still
    run every quantum, their output unused, so they are current when the
    arrangement comes back: on a plan that keeps the pitch, and after a
    switch to tape while the old converter is still read (until the switch
    and through its crossfade). A skipped converter refused the jump and
    failed every block after. Within the kernel's reach before a switch
    that lands on such a clip's wrap, live reads the pass's end gated at the wrap while the
    commit reads on (the switch reaches the node at its frame): a stated
    limit, measured up to 0.011.
  - *Under a playrate that keeps the pitch* (S3 step D) a launched audio
    clip reads through a stretch converter. A node with audio clips has
    three readers: 0 plays, 1 goes out (with its converter's history), 2
    warms the next launch up. A launch reaches the node `launchWarm`
    before its switch (the converter's output lookahead rounded up to
    quanta, and one quantum to spare: 2944 frames, about 61 ms, at
    48 kHz; `prewarm`; the switch's frame is kept as the slot's stream
    target, without the node's shift, so a plan that moves the shift during
    the warm-up keeps it, and is taken onto the trajectory's device frames
    by the plan's `rateOrigin`, which a seek moves), and reader 2 seeks there for output from the
    switch's quantum and primes once per 64 frames played (a frame
    credit, at any block size), so the switch costs no burst. At the switch the
    readers rotate (`applyLaunch`). Every prime position is a function of
    the switch's quantum and the trajectory, not of when the prime runs,
    so the warm-up is the replay's, which takes it as a third switch
    entry. A reading runs every quantum in order from its start, a
    quantum a block skipped (no weight there) before the next, up to 32
    quanta. With no warm-up (a launch from stop, a relocation, a plan that
    lost the converter) it seeks and primes at once: from its switch's
    quantum, catching up, when that is at most 32 quanta back, so it plays
    what a warm-up would, else at its own quantum; its loop cycle is the
    switch frame's either way (`Node::launchSwitchAt`, set by the first
    block after the switch, with its song phase, `launchSwitchPhase`,
    taken there: the timeline's ring keeps about 11 s at 48 kHz, so a
    restart long after the switch cannot read that frame back, and the
    cycle is that phase's under the loop as it is then). The cycle matters
    while the launch is anchored, before its first wrap, where the
    converter reads ahead past the wrap; a reader is not reseeked when its
    launch unanchors. The worst cold restart is 45 primes and 32
    quanta of catch-up in one block (48 kHz). It reads the player's song
    frames from the anchor, in loop cycles after the switch's; the switch
    crossfade and
    the clip's fades weight its output. Going out it plays on as the clip
    repeats, without the tape path's held pass or cut at the crossfade's
    end: its converter has read some 5000 song frames past a switch before
    the switch is known, and no capture commits it (Q2 b). Placement keeps
    an audio clip's launch `launchEarly` (the larger of the clip-chain
    lead and `launchWarm`) ahead of the clock (MIDI clips, stops and
    returns keep the clip-chain lead alone), and at least
    `launchWarm` after the track's applied audio launch still to switch
    (one warm-up at a time; a launch is applied from its warm-up's first
    frame, so a later one does not replace it), and any other launch of the
    track at least a frame after it, so the later press wins (round 139
    S1). A plan swap that
    keeps the rate controls hands the readers over with their converters
    (`carryLaunchReaders`, §1.6). One that changes the trajectory (a
    scalar, preserve pitch, an envelope; S3 step D4) switches launched
    audio where the timeline's items switch, at `rateSwitchDevice` (S, a
    device frame; in an executor's stream `handAt + S - rateHandDevice`,
    as `rateSafeStream_`), and the same way:
    - At adoption `carryLaunchAcross` (from `carryRateSources`) hands
      each reading to the new reader's `previous`
      (`PlayerReader::previous`, allocated at compile) and seeks the new
      reader for output from S. The old reading plays on the old
      trajectory (`previousTimelines`) until S and crossfades into the new
      one over `smoothFrames`, as an item does, converting while it is
      read. The new reader is primed at its first use by the executor that
      runs the node (the renderer for a rendered node, the callback for a
      live one): at most a warm-up's primes per reader in one block (45 at
      48 kHz), so the block holding S primes 45 for each track with
      launched audio under stretch, on the renderer within its
      render-ahead, and inside the device callback for a live node or with
      the in-callback executor (a known limit, STATUS). One first read more
      than the catch-up (32 quanta) after S starts again where it is read,
      as a primed reading does, rather than running every quantum since S
      in one block. A warm-up switching after S
      starts again on the new trajectory, primed as far as it had come, in
      the adoption's block; only a warm-up switching at or before S goes on
      in `previous`, and the reader rotated onto reader 2 at a switch drops
      its `previous` (round 152 F1). A tape reader keeps no loop cycle, so
      a reading handed on from one takes its switch's (`switchCycle`,
      from `Node::launchSwitchPhase`, recorded in the switch's block while
      the timeline still holds that frame; round 152 F2). A mirror that
      primed those readers on the main thread at prepare (phase 2) was
      built and taken out: it was dormant with the anticipative renderer,
      the default (ADR-0097, revision 2026-11-24, "the mirror taken out").
    - *A change of preserve pitch with a launch switching in (adoption,
      S].* Stretch to tape: reader 2 holds a tape reader with no warm-up,
      so the launch is not rotated in: the clip plays on tape from its own
      switch, not on a stretched warm-up until S. Tape to stretch: a tape
      plan warms nothing, so the clip plays on the new trajectory from its
      switch, primed there (step D's cold start). Neither crossfades at S;
      the launch's own crossfade covers its start (round 152 N2).
    - A switch inside that crossfade, in (S, S + `smoothFrames`), rotates
      the reader with its `previous` (`applyLaunch`): the outgoing side
      plays both until S + `smoothFrames`. An outgoing reader still in
      its own switch crossfade at S finishes it on the old trajectory.
      Placement keeps a node's switches `launchWarm` apart, longer than a
      crossfade (at most 960 frames, 240 song samples at 0.25), so one
      extra reader state per node holds every case. A plan whose
      `rateSourceAhead` exceeds `launchWarm` minus 960 minus 63 frames (a
      take chain of 40 ms or more) gives each such node a second.
      Both are allocated at compile.
    - The old trajectory is kept until every reader that reads it is
      done: when the outgoing or the incoming plan has audio players
      under stretch (a switch to tape included), `rateSafeDevice` is
      S + max(`smoothFrames`, ceil(`launchFadeFrames` / 0.25)) +
      `rateSourceBehind` + 64, so the next change cannot be adopted while
      an outgoing reader still reads the old trajectory (about 10 ms
      later than items alone need, at 48 kHz).
    - Nothing is freed on an audio executor: a primed reader the node
      does not take, and an old state once its crossfade is over, are
      swapped into the outgoing plan and released with it on the main
      thread, as `carryRateSources` does for items.
    - The read can be overtaken only by a launch already placed when it
      ran and applied before the adoption (a live node's warm-up, which
      the callback starts some `aheadFrames` after the renderer would),
      or by one replaced or voided: a warming launch placed after the
      read cannot switch at or before S (below), and any other only
      takes the audio reading away. The key check catches every case,
      and the miss path plays what a match would have.
    - No launch target already acknowledged moves. A warming launch
      placed from the prepare until the slowest executor that runs the
      node (for a live node, the callback's played stream) has passed S
      plus `smoothFrames` switches at least `smoothFrames` plus
      `launchWarm` after S, so its warm-up runs on the new trajectory
      alone. A quantized press can then land a whole boundary later.
    A switch to tape plays the old stretch reader as `previous` through
    the crossfade, converting while it is read. A switch from tape primes
    the new reader and fades from the tape one, which keeps no history;
    a launch the tape plan applied with no warm-up (tape has none) and
    which switches after the adoption takes the miss path: the executor
    primes it at adoption (for S, or as far as its warm-up's credit
    stands), within 45 primes, or it warms as usual when its warm-up has
    not begun, so it does not start cold at its switch.
    An audio clip read through a take chain (take
    FX or an active take envelope) still has no player there: compile
    warns and its launch is refused with `launch.playrate`.
  - *Capture* runs in song time (above): placement gives each launch its
    song frame, the boundary's unwrapped song sample, and the capture
    counts song samples from the published rate clock, so a commit lays
    each launch on its song position whatever the rate did. The commit
    meets the rate-1 commit's standard: exact away from the switches'
    crossfades, within 1e-6 through them (the float32 switch gain). Within
    the tape kernel's reach of the capture's first and last samples (128
    song samples each), where its items begin or end while the performance
    went on, it differs by up to the level of the audio cut there (a stated
    limit, measured and bounded by the tests). Under a pitch-keeping rate a
    switch that cuts an arrangement audio item, which a stretch converter
    reads on past the cut live, ends the capture on its boundary (below).
  - *MIDI* (`rateLaunchMidi`, a clip chain's input, a rack's Notes feed):
    each event at the first frame whose song position reaches it; what is
    held ends where the song enters a new loop pass; a switch's first frame
    reaches back to the anchor (`ClipWindow::Anchor`).
  - *The switch crossfade* runs over the same 240 song samples as at
    unity, from the anchor (`LaunchState::fadeStart`), each frame's share
    by its song position (`launchClock`), as a committed item's fade is:
    it is heard as 5 ms divided by the rate (ADR-0097, note 2026-11-22).
  - *Clip-time lanes and a clip chain's take envelopes* are not bound to
    the rate timeline; they read the clip position from the song position.
  - A frozen track's print cuts in on a playrate plan (no
    `retiringIntoPrint`), and a looped replay under a playrate stays
    refused (`render.loop`).
- **A clip take's chain (ADR-0097, amended 2026-11-20).** A clip whose
  active take has FX or an active envelope plays through a chain of its
  own, as an item's take does (`TakeChain::clipChain`, compiled with the
  timeline's take chains and linked to its player by
  `compileClipPlayers`):
  - *The lead.* A channel's `launchLead` is the most latency among its
    clips' chains. Every clip chain's input runs that far ahead; its
    output is delayed back by the lead less its own latency, so a clip is
    heard at its switch frame, not its chain's latency later.
  - *The early half of a launch.* A launch reaches such a channel
    `launchLead` before its frame as well (`prelaunch`, recorded in
    `Node::launchNext` until the launch applies). From there the incoming
    clip's chain reads it from its new anchor. A MIDI chain going out
    stops taking notes there and what it holds ends, so its notes end
    where the switch is heard. An audio chain going out reads on through
    the switch's crossfade, as a player's audio does.
  - *Running.* A chain runs while its clip is coming in, plays or fades
    out, and is otherwise idle. It is fresh (plug-ins reset, delays and
    held notes cleared) each time it starts, and runs on, unreset, across
    the clip's passes and a relaunch of the playing clip. Its output is
    weighed by the switch's crossfade as a player's audio is, with the
    clip's and the take's gain and pan after the chain.
  - *Its input over a transport loop* folds as the output will: an audio
    chain reads the loop start's song position (unanchored, as after a
    wrap), and a MIDI chain ends what it holds at the fold, as a timeline
    take chain running ahead does at its early wrap.
  - *The take's envelopes* (volume, pan, mute) are from the clip's start
    as heard: beats on a MIDI clip, seconds on an audio clip, from the
    start of each pass.
  - *Envelopes on its FX* are in clip time too (C2, schema 2.19): their
    lanes (`ParamLane::clipTime`) compile in the clip position's domain,
    unshifted, and read the clip position each stage's input will be heard
    at (`clipLanePosition`: the lead less the stages before it, folded as
    the input is). A lane sends on the absolute grid, at each point it
    crosses and at each new pass or anchor; with no input it holds.
  - *A MIDI take's instrument* in its chain plays the clip's notes, not
    the track's (`launchMidi` leaves them to it).
  - Plan swaps carry a chain by take id (its stages by instance, its
    delay while its length holds); the offline replay applies each
    launch's early half at its frame less the lead. A render without
    launches drops clip chains before `prepare` (`dropClipChains`).
- **The take `pitch` envelope** that is stored but never played (core
  §4.5) stays unplayed for clips as for items. Whether to play it is
  decided with the stretch path, for both.
- **Tests:** both executors against the offline replay of the engine's
  own launch log, at two block sizes, through the bridge, and under
  rtsan.

**Implementation (clip_player.cpp).**
- `compileClipPlayers` builds a channel's `Node::players` after its
  timeline events. A MIDI player's events are laid out over the period,
  one entry per pass of the loop window. Notes are cut at the window's and
  the period's end, and sorted by beat with offs before controls before
  ons. Their note ids continue the timeline's. Their note sources span the
  whole song, so `releaseVoices` never ends one as out of its span, and
  carried voices rebind by address as item notes do. An audio player holds
  the take's prepared buffer, gain, pan and channel mode, as an item does.
  `prepareRackMidi` sizes the track's MIDI pool and block window for the
  players (`clipVoices`, `clipWindow`).
- `Node::launch` is what the channel plays. `applyLaunch` changes it at a
  block boundary and sets `releaseHeld`, so the notes left sounding end at
  that block's start, as at a loop wrap. The executor splits its block
  there. That boundary is the channel's song position at the anchor: the
  launch's frame plus the node's `timelineShift`.
- Each block, `launchAudio` computes the arrangement's gain per frame,
  null when the arrangement plays throughout (the path is then exactly
  the old one). It adds the clip's audio, crossfading by song position
  over `kLaunchFadeSeconds`. Plain items and take chains are scaled by
  that gain. Timeline MIDI passes unplayed while a clip holds the track,
  and `launchMidi` emits the clip's notes instead. A clip's notes join
  the track's MIDI stream, so its MIDI sends carry them.
- **A rack's Notes module follows its source track** (revised
  2026-11-20). Its feed keeps its own launch state and its own players
  for the source track's MIDI clips (`compileFeedClips`), switched by the
  same launches at the rack node's frame (below). While a clip, a stop or
  a held clip holds the source track, the feed's timeline events pass
  unplayed and `feedClipMidi` gives the clip's notes, with the phase rules
  of `launchMidi`. At each switch the notes the timeline or a clip held
  end (`releaseHeld`); an audio clip leaves the feed silent. Wraps,
  relocations and plan swaps treat the feed as they treat the channel.
- `OfflineSettings::launches` replays a log from the render's start. With
  `OfflineSettings::loop` the render wraps as the live executors do: no
  block crosses the loop end, the song continues at the loop start
  (`wrapTo`), the take chains wrap early in that loop, and the running
  frames the switches are placed at count on. Not under a master playrate
  (`render.loop`).
- **The queue (launch_runtime.cpp).** `Engine::launch` places a set of
  requests (a scene's tracks) on one target. The floor is the latest of
  its tracks' floors: the published position's stream plus one device
  block for a node the callback runs, or the renderer's stream plus one
  plan block for a node it renders. The target is the first quantize
  boundary at or after the floor's song position. A loop end before it
  places it at the first boundary inside the loop. Stopped, the target is
  the play position, applied at the play start. Right after a pause or a
  seek, before the anticipative renderer has relocated, the play position
  `transport()` reports (and so a launch's anchor and a capture's start)
  is where it will relocate: the seek's sample, else where it paused
  (review round 70). The queue has
  `kLaunchSlots` fixed slots, one per track; a newer request replaces the
  track's pending one, and a full queue refuses with `launch.queue`.
  Each slot's state word carries a generation, so a slot the main thread
  rewrote meanwhile is never applied.
  - **Targets (ADR-0097, revised 2026-11-20).** An entry switches the
    track's channel (when it has players) and every rack Notes feed whose
    source is the track (`NotesFeed::follows`: the source track has clips
    and plays them). Each target switches at the stream sample `target +
    timelineShift` of its own node, in the executor that runs that node;
    each executor cuts its block at its targets' frames. The floor counts
    every target's executor. A channel with clip chains is a target
    `launchLead` early as well (`prelaunch`): its floor, its horizon check
    and the frame that decides the entry are that much earlier (ADR-0097,
    amended 2026-11-20).
  - **One decision for all of them.** States: pending, committed,
    applied, missed and voided. Each executor class (the renderer, the
    callback) stores its horizon, the epoch and the stream its current
    block may reach, before it reads the queue (sequentially consistent).
    The main thread publishes an entry pending, then reads the horizons of
    the executors running its targets in the latest plan. If each is at or
    before the target (or in an older epoch), it commits the entry;
    otherwise it marks it missed. An executor that reaches, or is past, a
    frame of a pending entry marks it missed. So a committed entry is read
    by every executor running a target before it passes that target's
    frame.
  - The first target of a committed entry to switch moves it to applied
    by compare-and-swap. An executor past a frame of a committed entry, or
    whose plan lacks the clip at the track's frame, voids it instead, and
    `service()` voids a committed entry of an ended epoch: nothing of a
    voided entry switches. An applied entry switches each remaining
    target at its frame. An executor that relocated before reaching one
    switches it at its next block, marked as after a relocation. Each
    executor sets its bit in the slot's `done` when it has nothing left of
    it; the slot is free again once every executor class has.
  - Only the track's switch sends the ack and joins the log; a feed's
    switch follows from it, in the offline replay as live.
  - An entry of another epoch is not applied by an executor in a newer
    one, except the remaining targets of an applied entry (above).
    `service()` places missed, voided and old-epoch entries again, alone,
    from the current position. Per placement, a launch the renderer
    overtook while it was placed lands at most one service interval (the
    app's tick, or a headless host's own loop) plus the floor after it was
    pressed when unquantized, and on the next boundary after that
    otherwise. The floor is the render-ahead, a block, its lead or
    warm-up, and at least a frame after the track's applied warming switch
    (above). A re-placement can be overtaken in turn, so under sustained
    overload the delay is a whole number of intervals; while relocating,
    `launch.later` defers it to the next service. It drops, with a
  warning, any waiting entry whose track or clip the latest plan cannot
  play (frozen, removed, or a master playrate active), so an edit never
  leaves a launch to fire later. Applied entries come back
  through a ring per executor, become `LaunchLogEntry`s and are kept in
  `launchLog()` in running-frame order.
- **Renders.** An offline render without `OfflineSettings::launches`
  compiles no players (`Plan::clipPlayers`) and prepares no clip audio:
  renders play the arrangement only, so a clip's warnings never reach a
  render or block a freeze. A clip past the memory budget gets no player
  either (`Engine::clipsSkipped()` names them; `launch.memory`).
- **What a track plays, for the GUI and `launch.state`.**
  `Engine::launchState()` answers, on the main thread, from the log and
  the running plan. It reports each track's newest heard entry, where an
  entry not yet reached by the played stream is still queued, along with
  its pending slot. A track whose clip a later plan lacks is back on the
  arrangement from then on.
- **What a press would be refused for, for the launcher pane** (step E).
  `Engine::launchRefusal(request)` runs `launch()`'s checks of one
  request against the latest published plan without placing anything,
  and `launch()` runs the same function first: `launch.playrate` (an audio clip the
  plan gave no player because it reads through a take chain while the
  pitch is kept: `Plan::launchRefusedPlayrate`, recorded by compile),
  `launch.memory` (`clipsSkipped`), `launch.clip` (no players on the
  track, or the plan lacks the clip). The app builds each request from
  the session the commands read, not the engine's (an edit can be on its
  way, as while audio is prepared), and gives every refusal the path a
  caller of the press receives, `/args/clip` or `/args/scene` (the bus
  puts `/args` before a command error's path), so the answer is the
  press's error field for field.
  `EngineApp::launchRefusal(clip)` puts launch.clip's own `track.frozen`
  check first, engine or not;
  `EngineApp::sceneLaunchRefusal(scene)` makes launch.scene's selection
  (`sceneLaunchRequests`: frozen tracks' and memory-skipped clips left
  out) and answers `launch.scene` when it is empty, else the first
  refusal among its clips. Transport-state refusals (`engine.stopped`,
  `launch.later`, `launch.queue`) are not reported.
  `EngineApp::launchRefusals()` gives every clip's and scene's answer in
  one pass (`clips` and `scenes` maps, refused ones only), equal entry for
  entry to the per-id calls: it reads `clipsSkipped` and the display order
  once and takes every scene's selection from `sceneLaunches`, the one
  function launch.scene and the per-scene answer also take theirs from
  (round 149 S1), so a pane's recompute costs O(C log C + T), a scan of
  the memory-skipped clips per clip, and each clip's plan check (round
  147 F1).
  `EngineApp::launchRefusalGeneration()` moves whenever an answer can
  change: a published plan (each engine's count is folded into a base
  that survives a restart), an engine starting or stopping, a session
  version (track.freeze sets its flag before its plan arrives, an undo)
  and a project opened. It never repeats a value.
- **Plan swaps.** `carryMix` carries `Node::launch` by clip id
  (`carryLaunch`). A clip the new plan lacks, or a node with no players
  (a frozen track), returns its track to the arrangement at once, and its
  notes end.

### 1.5 Transport and time

The plan holds a sample-domain time map (tempo segments → samples) so the RT
thread computes CLAP transport (beats, bars, tempo, time signature, loop,
playing/recording flags) per block without floating-point drift (positions
are integer samples; beat positions derived per block from the segment
table). Looping wraps at exact samples; a block that crosses the loop point is
split.

Time conversions clamp at their representable limits: `samplesAt` saturates
rounded products to signed 64-bit samples (NaN maps to zero); `barBeat` clamps
to signed-int bars including the last fraction of the final bar (NaN selects
beat zero). Callers must still validate finite positions and leave room for
block and latency arithmetic. JACK validates requested rates in 1..768000 Hz
before constructing its latency request.

### 1.6 Plan rebuild and swap

Every compile is complete: `Engine::load` builds a whole new plan from the
session, every node, stage, buffer and event list, except for the edits that
publish values instead (mix values below, ADR-0060, and the playrate scalar,
ADR-0046) and patch-only FX edits (below, ADR-0092). Plugin instances are not rebuilt: the instance pool keeps each
FX's instance while its plug-in and isolation stay the same, and applies a
changed patch or state to it on the main thread during the compile
(`InstancePool::get`). State crosses the swap when the executor adopts the
new plan, matching nodes by id: fader, wet and Console 9 ramps
(`carryMix`), held notes (`carryLiveNotes`, `carryRackMidi`), each
modulated parameter's last sent `PARAM_MOD` value (§1.4.1), and delay
histories, a rack's plug-in stages' (wet ramp, dry delay, oversampling
filters) as a chain's. Held timeline notes carry only when the receiving instrument
instance is retained. Route and parent edge delay histories carry when
source, destination, tap, kind and delay length match; changed lengths start
empty. New plugins are instantiated and activated on the main thread before
the swap. Edits are being made cheaper in stages (route E: rings, then the
FX identity test, then patch edits without a plan swap, each stage
specified here as it lands); so far:

- **Rings are recycled** (stage 1). A frontier or record ring of a plan
  both executors have left goes to a main-thread pool when plans are
  collected, and the next compile takes a ring of the shape it needs from
  there, resetting its indices; old chunk contents are never read, since
  the producer writes every field of a chunk before publishing it. A ring
  of a plan an executor may still adopt or play is never reused. The pool
  keeps as many rings as the plans still in use hold. Every ring has room
  for the render-ahead time at 192 kHz in chunks of the render quantum,
  plus two. At lower rates that room is the slack for short chunks: no
  chunk crosses a loop wrap, so a loop shorter than the quantum makes every
  chunk that short. The renderer still gets its whole lead with a loop down
  to the render-ahead time divided by the ring's chunk count (about 3 ms
  with the defaults, at any rate); a shorter loop holds it closer to the
  callback. A recycled ring is not zero-filled again: its pages were
  committed when it was first built.
- **An unchanged FX is not serialized** (stage 2). `InstancePool::get`
  remembers the session's FX object it last configured an instance from
  (and keeps it, so the address is not reused). Given that same object
  again it knows the patch and state are unchanged; given another, or a
  copy that is not the session's, it compares the patch's canonical text
  as before, and applies that text when it differs. It forgets the object
  when it recreates the instance and when the app acknowledges a patch or
  state the plug-in produced, since the object then no longer describes
  the instance.
- **A patch-only edit publishes no plan** (stage 3, ADR-0092): see
  "Patch-only FX edits" below.
- **A context-only edit publishes no plan** (ADR-0095): see "Context edits"
  below.
- **A plan has a canonical dump** (stage 5; `plan_dump.h`). `dumpPlan`
  (main thread, before publication) writes every field of a prepared plan
  that compile and prepare decide, one per line in plan order: nodes and
  their stages, lanes, latencies, pins, delay lengths and buffer sizes,
  events, clips and take chains, edges, racks, the live and frontier sets,
  the task graphs, the instances (named by FX or module, never by address)
  and the mix slots. Runtime state (cursors, ramps, voices, held notes,
  queues, buffer contents, meters, the executors' handoff fields) is left
  out. Each struct it walks is first decomposed into all its fields by name,
  so a field added to `Plan`, `Node`, `Stage`, `Edge` or the others stops
  the file compiling until it is dumped or named as runtime state. With
  `EngineOptions::verifyPlans` (tests; off by default) the engine compiles
  each plan a second time and fails the load with `engine.plan_differs` and
  the first differing line when the two dumps differ, publishing nothing
  (a test changes the second compile to see it); the plan-swap soak runs
  with it on. A compile services the instances it takes, so a plug-in
  restart asked for or completed between the two changes the second: the
  pool counts those changes (`InstancePool::availabilityChanges`), and when
  the count moved the later compile becomes the plan and is checked against
  another, up to three times, instead of refusing the load. This is the
  check a partial compile (stage 7) will be held to. The soak also plays its sequences again on a manual device with
  two plan swaps that change nothing after each edit, a marker added inside
  the edit's ramps and removed later, and wants the output unchanged sample
  for sample in both executors: whatever a swap fails to carry is heard.
- **Carries look nodes and edges up by id** (stage 6). The compile indexes
  each plan last (`indexForCarry`, main thread): its nodes sorted by id
  (a plan has one node per id, asserted there in debug builds), each edge's
  source id, each node's in-edges sorted by source id (ties in
  `in` order). At adoption the executor finds a node's counterpart by
  binary search (`findNode`, `carriedNode`), and an in-edge's or a MIDI
  send's predecessor among the old node's in-edges from the same source, the
  first in `in` order that matches (`matchInEdge`, `matchSend`). These are
  the pairs the earlier scans of every node and edge pair found, whichever
  earlier plan the executor carries from (the callback can skip plans, the
  live region lags); the soak and a MIDI-send test compare them with those
  scans, the latter also with a key route retargeted to another FX.
  Nothing is allocated on the audio thread. At 192 tracks the callback's
  carry compares about 7,400 entries per adoption instead of 75,000; its
  time is the carry's own work, linear in the plan.
- **A compile's cost is linear in its plug-ins** (the compile profile's
  step 1, in place of stage 7). `prepare` takes the restarting instances
  from the pool once (`InstancePool::restartingInstances`, one pass, sorted)
  and looks each plan instance and stage up in that list; it asked the pool
  for each, which scanned every entry. A test counts the entries visited
  per compile (`restartLookups`); another makes two instances restart whose
  addresses run against their ids, the case an unsorted list gets wrong,
  and checks that both leave the plan.
- **Note sources are built by sorting, once per node** (step 2). A note's
  ordinal counts the earlier note-ons with the same origin, port, channel
  and key (a keyless note's written key, below): `prepare` sorts a stage's note-ons by that address and arrival
  and numbers each run, where it kept a map of counts per stage, and the
  chain's note target copies the MIDI stage's sources (the same events). A
  test compares every stage's sources and index with the map-based build.
- **A compile reuses retired plans' buffers** (step 3; `buffer_pool.h`).
  When `collectPlans` retires a plan (no executor can reach it, the rule
  its rings follow) the engine's `BufferPool` takes its stage, node, edge,
  rack-binding and record-tap buffers, its voice lists and its output event
  storage; the next `prepare` takes buffers of the size it needs from there
  and clears each as a new one would be (zeros, empty, default events), and
  allocates only what the pool lacks. After each collection the pool keeps
  as many of each size as the last `prepare` took. Offline renders allocate
  as before. A reused buffer never sounds: its contents are cleared on the
  main thread, and the executors also clear an uncarried stage's delays when
  they adopt a plan, so the first guard alone is checked at the plan: a
  retired plan's buffers, filled with garbage as they enter the pool, serve
  a compile of the same session that must hand each out fresh, take all of
  them from the pool and dump as a compile without it. Engine tests fill the
  pool the same way (`EngineOptions::poisonRecycledBuffers`) and compare runs
  with and without reuse (`recycleBuffers`) sample for sample in both
  executors, across plan swaps that carry a rack's plug-in stage, a newly
  inserted latent stage, seeks, and plans with fewer and more channels and
  strips; a steady compile allocates none. The cost is memory: between
  compiles the pool holds about one plan's worth of buffers, which were
  freed once a plan retired. In engine-bench's generated sessions that is
  70 MB at 96 tracks and 139 MB at 192, most of it (114 MB at 192) the
  voice lists, two per stage that takes notes with room for 4,096 voices
  each, whether or not a plug-in holds one. It does not grow: a test
  compiles at block sizes from 32 to 1024 in turn, and a one-channel
  session between compiles of the rich one, retiring each plan as the next
  goes live, and checks that the pool never holds more audio buffers, voice
  lists or output event buffers than the last compile took.
Returning to a version with neither patch nor state after applying saved FX
settings recreates the factory-default instance. The old instance remains alive
until both executors release its plan, as with a changed opaque state.
The swap: `std::atomic<const RenderPlan*>` exchanged by the model thread;
the RT thread picks up the new pointer at the next cycle start and
acknowledges via an epoch counter; the model thread destroys the old plan
(and deactivates removed plugins) only after the acknowledgement. Nothing on
the RT thread frees memory.

Pooled rack modules also carry exclusive executor owner tokens (rack.md §6).
Each plan prepares sorted module lists for all nodes and for each executor.
After its previous job's helpers join, adoption releases modules absent
from that executor's new list and atomically claims free modules. Block
boundaries retry claims without waiting. A rack runs only when the caller
owns every module; unavailable racks emit silent audio and no Note Out
events without touching shared bindings or DSP state. Locates defer shared
Notes/Note Out resets to the next owned processing call. This covers an old
live plan overlapping a newer renderer plan while stopped, when monitoring
is disabled and `keepLive` does not apply, as well as rapid plan handoffs.
The callback-only executor uses the same ownership rule for the whole plan;
offline instances are private. Normal processing keeps the same grid,
summation and samples in both executors and offline.

`engine-plan-soak.tests` exercises this contract with seeded command sequences
while a paced dummy audio thread plays, in both executors with two helpers.
Its `engine;rt` CTest entry runs in the normal and sanitizer presets. Every
command checks model invariants and nondecreasing per-branch ID counters;
after source preparation and plan adoption, every step checks finite, bounded
device output, process errors, and reclamation of old plans and replaced FX
and rack modules. The graph, pending handoffs and retained-plan capacity are
bounded independently of the long-run duration. At the start and after every
settled edit, a 1024-frame intermediate window is compared bit for bit with a
private offline render of the current project, in both executors. The dummy
driver parks between callbacks, outside the RT entry, then runs fixed callback
counts on its existing paced audio thread with renderer readiness handshakes.
The engine and pooled instances stay running. A locate supplies the defined
DSP reset boundary; before it, four complete 128-frame callbacks after parking
drain the specified 10 ms swap/wet fade. Adoption alone can acknowledge at a
block's start, so an earlier cycle-count observation is insufficient.
Window anchors cycle through beats 0, 1, 2 and 3 of the
current tempo map. Output latency is accounted for, and the preceding loop
and position are restored afterwards. JSONL checkpoint rows preserve exact
sample windows when replaying or shrinking an edit prefix. These probes cover
intermediate project states after a locate, not arbitrary uninterrupted DSP
history or the pre-locate plan-swap crossfade.

Each epoch stops playback,
compares an offline render with a fresh app reopened from the saved final
project, and checks the retained RT pool against that reference bit for bit.
Unwinding both branches restores the original canonical project content,
excluding only `project.next_ids`, whose high-water marks intentionally survive
undo (ADR-0008). The seed determines the operation stream; native thread
interleaving remains free. Readiness, callback counts and sample values decide
correctness; elapsed time does not. A 60-second handshake watchdog exits 124
with `WATCHDOG ABORT` and no audio-mismatch verdict, rather than asserting or
continuing the diagnostic run. Wall time also paces the driver and selects an
opt-in long-run budget, without deciding a sample comparison. Long-run and
replay controls are in `tests/fuzz/README.md`.

Reclamation checkpoints observe both executor acknowledgements before
collecting on the model thread and require the published generation to stay
unchanged through collection. A callback acknowledgement arriving after an
earlier collection does not imply that collection already freed its old plan.

**Mix-value edits (ADR-0060).** A session that differs from the last one
`Engine::load` took only in strip `volume_db`/`pan`/`width`/`pan_left`/`pan_right`,
send `volume_db`/`pan`, VCA `volume_db`, and since ADR-0093 FX `wet`/`bypass`
(not `offline`, which decides whether an instance exists), track `mute` and
`solo`, VCA `mute` and `solo`, a modulation route's `depth` and `enabled`
(ADR-0096), and route `mute` where the send is encoded
anyway (a tap before the fader, a level other than 0 dB, its own pan, mono
or polarity; MIDI routes and rack sends are never encoded) (and in the
points of envelopes a write pass holds, including the new pan targets,
§1.4) is not compiled. The comparison copies each changed track, route,
group, FX and envelope with those fields taken from the new one and
compares the whole object, so any other change (a name, an item's mute, an
FX's plug-in or pins, a route's tap) compiles. A send whose level moves to or from exactly 0 dB, or whose pan
starts or stops following its source, compiles: that decides whether it is
encoded. A rack send is never encoded, so any change of its `volume_db` is
a mix value (ADR-0075). An unchanged session is a maintenance reload and compiles too. A
qualifying edit writes the values into the newest plan's mix slots (per
node, per route edge, per VCA, per stage for wet and bypass, numbered node by
node in `forEachStage` order from `Node::stageSlot`; parent sends carry no
values) between an odd and an even sequence number. Each executor, at the start of a block
(`syncMix`, `SUPPLE_RT`), copies the slots its nodes and edges read when the
sequence moved: a node's own values; an edge's for its encoder (source side)
or, for a key route, its destination; the source level a send's static gain
needs comes from the slot, not the other executor's node. The static gains
come from the same functions compile uses, so the plan then holds exactly
what compiling the session would, and the fader ramps carry the change over
the smoothing time as a plan swap does (an edit while playing is
bit-identical to a compiled reload at the same block). A copy that raced a
publication keeps its old sequence and repeats at the next block: for that
block the executor can play a mixture of the old and new values (each a
whole atomic value, ramped as usual), and the plan matches the compiled
session from the first block whose copy completes. The edit
publishes no plan, prepares no rate transition and changes no epoch, also
after rate scheduling has started (§ Master playrate); a plan still queued
for adoption takes the values when it is adopted.
A solo edit publishes each strip's silenced flag, from
`model::soloSilencedTracks` (the function compile uses), and a rack send
copies its home channel's mute and flag. A stage's wet and bypass reach it
the same way, and its own wet ramp crossfades the change as after a compile. No compile-time decision depends
on them: a stage's latency, dry delay, MIDI bus slot and note target are
the same bypassed or not, and whether it takes part in the MIDI bus is
decided each block (ADR-0093). In the anticipative executor a rendered node
takes a value edit at the next block the renderer renders, and a compiled
reload hands over at the renderer's position. With the renderer full and
waiting for the callback when the edit is made, both start at the same
block, so an edit heard only on rendered nodes is bit-identical to the
compiled reload. A live node takes it at its next callback block, the
render-ahead rounded up to a render block earlier than the reload reaches
it, and the two are identical once the reload's ramp is done. A renderer still filling renders with the
old plan while the compile runs, so a compiled reload is only ever later.
In the anticipative executor (`detail::changedExecutors`, ADR-0093) an edit
compiles when it changes these values both where the callback reads them and
where the renderer does (a node's executor, an edge's owner, a hardware MIDI
route's source), or where the callback reads them while an earlier edit the
renderer reads is still ahead of the playhead: the callback has not yet
played, in that epoch, to where the renderer was rendering when it was
published plus one render block. The two take values at different stream
positions, and in between would play neither session's mix, such as silence
when solo moves from a monitored track to a rendered one, in one edit or
two. ADR-0060's level and pan edits and ADR-0092's patch edits are not
gated.

**Context edits (ADR-0095).** The plan does not read the context track (the
`cx` collection), so a session that differs from the last one `Engine::load`
took only in that collection is not compiled: the `cx` storage differs, every
other collection, the project, the blobs and the unknown sections have the
same storage, and `latest_` takes the new session. It is a shortcut of its
own and, like the others, runs only when nothing forces a compile, and never
while an edit is deferred or for the reload that ends a deferral (the guards
of `loadScalarRate`; there `latest_` is the deferred session, and a session
identical to it is not a context edit). It is tried before the scalar, mix and
patch ones, and its list of `Session` members is bound in the code as theirs
are; the three equivalence checks do not list the
context, because each of them qualifies a fast path only when something it
handles changed, so an edit that changes nothing they handle falls through
them to a compile. A context edit that comes with any other change compiles
as that change does. When something in the plan reads the context (a note
source, a rack module), an edit compiles only if the plan reads it.
*(Amended 2026-11-21, ADR-0098 "Amendment: following is playback": while a take
or a clip in the session follows, the plan reads the context, and a context-only
edit compiles; see "Following at playback".)*

What the tests pin, and what they do not (the reader of `loadContextOnly`'s guards
should know which is which). `a context-only edit compiles nothing and the same
session again does` (`test_mix_edits.cpp`) pins: a chord edit, and an edit of a
point's content and a new point together, compile no plan; the same session again is
a maintenance reload that compiles (the shortcut never takes it: the `cx` storage
does not differ); a context edit together with a track edit compiles as the track
edit does. `a context edit made while a structural edit is deferred does not swallow
it` (`test_module_library.cpp`) pins the outcome: with a future non-unity plan and an
FX removal waiting, a chord edit compiles nothing, and the removal still compiles and
plays once its span has passed, with no engine error. **It does not pin the deferral
guards.** The test also passes with the guards removed: the shortcut adopts the whole
new session as `latest_`, which holds the waiting edit, and the deferral's own retry
compiles it either way. Both tests fail when the "`cx` storage differs" check is
removed. No test makes the guards alone matter. They are kept so the shortcut has the
same guards as the three it sits beside. They are redundant by construction (round 57
review: removing every one leaves the whole `engine.tests` suite passing, and the argument
holds): every path they protect reloads `latest_` itself, the reload that ends a deferral,
a rate seek or loop and the recovery seek each load `*latest_`, and a session whose `cx`
storage is the running one's is refused by the storage rule, so the rule does what the
guards were first thought to do. A later change that made adopting `latest_` insufficient
would have to revisit them; no test stands behind them.

**Patch-only FX edits (ADR-0092).** A session that differs from the last one
`Engine::load` took only in FX `patch` values, each of those FX having a
patch before and after, is not compiled. Every other collection must have
the same storage, and each changed FX is compared whole with its patch taken
from the new one, so a change of anything else (wet, bypass, pins, a
plug-in, a name elsewhere) compiles; an unchanged session compiles. The
edit also compiles when a guard of the mix-value path holds (a deferred or
rate-seeking edit, a queued scalar, a notification reload), when an edited
FX has no instance the pool can patch (none, failed, waiting on a restart,
another plug-in or isolation, configured from a state blob), belongs to a
frozen track or has an active envelope (a new plan resends automated values
at its first block), or when any instance has automation its plug-in
cleared (a compile restores it). Otherwise `InstancePool::applyPatch` applies
each patch as `get()` would and services the instance; a rescan of the
parameter list (INFO, ALL or no flags; a values or text rescan is not one) or
a clear, a latency change, a restart request or failure, a change of
activation or of ports then makes it a compile, which finds the patches
applied. When the patch is heard is unchanged: it reaches the instance on
the main thread inside `load` as before, and a plan swap neither drops nor
re-renders chunks rendered ahead, so a live node plays it from its next
block and a node the renderer runs after the chunks it had rendered. The
real-time output is bit-identical to a compiled reload of the same edit at
the same block. The patch sequence (`Engine::patchSequence()`) moves by one
before the patches are applied and by one after; every executor reads it
before processing a block and stamps that block's plug-in outputs with it,
and the app drops outputs stamped with any value other than the current
one (see plug-in outputs below). `Engine::patchEdits()` counts these edits.

### Following at playback (ADR-0098 amended; the contract for X4a and X4c)

*X4a is built: the live tables, the budget, the note identity rules below for
clip notes, `launch.state`'s `followed` and `follow_budget`, and the
context-edit compile while a launched clip follows. X4c (the timeline mapping,
the plan-wide pool, the tables read as the model reads a context, the budget
warning in the launch answer, `followed` note origins and the context-edit
compile while a take follows) is built too.* What a note sounds as is `model::soundingKey`
(`session-model.md`, "Take context and follow"); the engine only places its
results.

- **Timeline items: at compile.** Item notes are compiled into each stage's
  timed events at plan time, as now. A following take's note-ons take their
  keys as `model::soundingNotes` (`session-model.md`) gives them: the compile
  walks the same passes, chased notes and crossing notes, takes each note's
  place from `model::notePlace` (its unwrapped source beat, the start rule)
  and its key from `model::soundingKey`. Each note-off takes its note-on's
  key. Nothing is added to the audio path, and there is no budget.
- **Launched clips: the live tables.** X4's tables keep their rows (128 keys,
  one per pair of places) in one pool per plan, deduplicated by content across
  all clip players; each clip's tables hold a reference to the pool and are
  read only with their own plan's project regions (another plan's would index
  other rows, out of bounds; no MIDI player retires into a later plan).
  A clip's places are read as `model::soundingKey` reads a context (its first
  key and chord hold before their first points; the project's nothing before
  its first). The budget stays per clip: its own pairs, counted before
  deduplication, at most `kFollowPairs` (1024); past it a `chords` clip
  follows its key, and past that it plays as written. A clip's outcome so
  depends only on the clip and the project, never on what else is launched.
  `launch.follow_budget` stays in the engine status's warnings while it
  applies (a status, not an event); `launch.clip` and `launch.scene` return
  it as a warning for the clips they launch, and `launch.state` marks the
  clip's entry `follow_budget: true`.
- **`launch.state`** (X4a): each listed track gains `followed: "none" | "key" |
  "chords"`, the outcome the plan uses for the clip it plays or has queued,
  omitted when it plays the arrangement or nothing. `launch.unfollowed` goes.
- **Note identity.** A note origin gains `followed` and `written` (its source
  note's written key); the rules key on `followed` or on `kClipOrigin`
  (`NoteOrigin::keyless`). For such a note:
  - its address (the one note sources are sorted, numbered and carried by)
    holds its written key in place of the key it sounds at, so a plan swap
    carries each held voice to its own source, a chord's notes included,
    which share the rest of their address (review round 86 H1);
  - `prepareNoteIds` pairs its note-off by source id, port and channel, taking
    the held voice's key. It pairs a note-off the same way when the held voice
    sounded keyless, whatever its new source is: a carried voice keeps the
    origin it started with, so a take that stops following while its note is
    held still ends the key the note sounds at (review round 87 J1).
  So a context edit while a note sounds lets it play on to its note-off at the
  pitch it started with, and so does a change of following (on to off, off to
  on, key to chords, take.set or an undo): no note sticks, and none is cut
  early unless the note itself is gone. A MIDI send's voice already ends at
  the key it delivered.
- **Context edits.** A context-only edit takes the compile path whenever an
  item's active take has `follow` other than `none` or the plan follows a
  clip; otherwise the shortcut above stays. Following costs the compile about
  2.5 µs a note-on (a skipped test in test_clip_player_follow.cpp prints it):
  compile and prepare of 400 one-bar items of 16 notes took 37 ms following
  nothing, 39 ms following key and 53 ms following chords (dev, Debug, median
  of 7), so no per-place cache is kept.
- **Everything downstream reads compiled events:** offline renders, freeze,
  stems, rack Notes feeds, MIDI sends and plug-in MIDI inputs take the sounding
  pitches with no change of their own.
- **Invariants.** Real-time output stays bit-identical to the offline render in
  both executors and bridged. Tests (X4c): a render of a following item equals
  `take.bake` and then a render as written; live equals offline across a
  context edit, in both executors; a note held across a context edit ends at
  its note-off, at its start's pitch; a looped item whose passes sound
  differently; a pooled source read by a following and a written take; a clip
  over its budget beside one within it, each with its own outcome; the
  compiled keys equal `soundingNotes`'s, a chased note's included; before a
  clip's first chord point it follows by the first chord, launched as laid;
  timeline
  equals launcher for the same clip (a clip of length 8 over 4 beats of looping
  content, C at clip beat 0 and Eb at 4, laid and launched, also at a take
  playrate of 2); a chased note maps by its own start.

## 2. Real-time execution

### 2.1 Live and anticipative regions

A node is **live** when it consumes monitored hardware audio/MIDI or armed
audio input (including an input FX chain). Live classification propagates
downstream, including sends and the master. A MIDI edge, an auxiliary link
and a modulation route (§1.4.1) also pull their source live when their
destination is live, since the frontier rings carry only main pairs. All other nodes are anticipative.
Opening a plug-in GUI or MIDI editor does not currently change classification;
editor/automation eligibility preferences remain planned (ADR-0014).

- **Anticipative nodes** run on the renderer and background worker pool in
  larger blocks (default 1024 samples), up to **200 ms** ahead. A nonsplit
  graph publishes its master into an SPSC ring; a split graph publishes each
  renderer-to-live frontier edge into a ring. The callback consumes those
  buffers at device block size. Parameter edits can take effect at that
  horizon. A seek/structural edit establishes a new epoch and prerolls;
  starvation after priming counts as an anticipation miss.
  **Renderer waits (ADR-0028):** while stopped, its share processes at the
  next render-block deadline. A process/flush request or queued parameter edit
  can bring that deadline forward, to at least 1 ms after the previous stopped
  block finishes. A notification by itself does not cause an extra block.
  While playing, the callback wakes it once enough audio has been consumed
  for another block. A full master/frontier ring must first release a chunk.
  Missed-wake insurance is half the effective render-ahead span during
  playback (normally 100 ms). It is 1 s while stopped, held, planless or all
  live. Plan and transport changes, hold/release, shutdown, callback epoch
  acknowledgement and instance ownership release also wake the renderer.
  Bridged requests arrive through existing main-thread service.

  `rt::waitUntil(word, seen, deadline)` waits off the callback until the
  sequence changes or its steady-clock deadline expires. It rechecks after
  interruptions and returns true on change, false on expiry.
  `Instance::setAudioWake(word*)` subscribes on main; null detaches and waits
  for outstanding notifications before the word can be freed. Plan retirement
  and engine stop detach subscriptions. `processRequested()` also reports
  queued active VST3/bridged `setParam` edits; `takeProcessRequest()` consumes
  them together with any CLAP request.
- **Live nodes** run on the callback and its separate bounded live pool
  (ADR-0024). With anticipation disabled this executor runs the entire graph.
  Limit 1 processes it serially. Bridged stages on live nodes are processed
  there too, under the cycle's bridge deadline (§5, ADR-0081).

### 2.2 Worker pool

The background/offline pool has `max(0, cpus − 3)` helper threads by default,
where `cpus` counts the CPUs in the calling thread's affinity mask (taskset,
cpusets), not every online core; its submitting thread also executes tasks. `EngineOptions::workers` can
override the helper count. A bounded MPMC ready queue and atomic dependency
counters distribute nodes; input summation stays in fixed graph order.
ADR-0023 bounds each run to the smaller of the configured capacity and a
conservative graph-width bound: a greedy disjoint dependency-chain cover.
Only selected helpers are notified through individual sequence slots. Chains
run serially on the submitter without queue operations. During parallel jobs,
participants park when no work is ready; successor publication and final
completion advance an atomic event, observed before checking either predicate
to prevent missed wakes. There is no application spin/yield loop. Enrollment
precedes publication, and the submitter waits for every selected helper's
departure before reusing job state or returning, including preempted helpers.

`EngineApp::setAudioThreadLimit` sets the app default (0: Automatic, 1..64:
maximum execution threads per pool including its submitting thread). Invalid values return
`engine.audio_threads`, path `/audio_threads`, with a corrective hint. The GUI
persists this as `audio_threads` in preferences (§6 of gui.md). The next engine
start, including project replacement and device recovery, maps a nonzero limit to `workers = limit - 1` when
options request automatic workers; explicit `EngineOptions::workers` takes
precedence. Both live and background pools use this capacity independently; it is not a
total process thread limit. Already running pools and offline export's
`threads` option are unchanged. Automatic retains the CPU-mask-derived capacity and graph-aware
participation; an explicit limit is also graph-bounded.

DSP tasks are RT-safe; ADR-0013 separately permits synchronous bridge futex
wake/wait with a failure deadline on background threads, never in the device
callback.

**Scheduling (ADR-0026).** The backend supplies the device callback's
scheduling, and the engine never changes it. The engine derives the other
classes from it, all process-local:

| Class | Policy | PipeWire default | Rule |
|---|---|---|---|
| Device callback | the backend's | FIFO 83 | never changed |
| Live helpers | FIFO | 83 | the callback's priority (ADR-0024) |
| Renderer, background helpers, bridge audio threads | FIFO | 10 | `Engine::kBackgroundPriority`, lowered to the callback's − 1 and to `RLIMIT_RTPRIO` |
| Offline renders, recording writer, main/GUI, jobs | normal | – | unchanged |

- **Why 10.** It is below the kernel's threaded interrupts and display
  workers (FIFO 50) and the audio threads rtkit grants other applications
  (up to 20). Any real-time level outruns normal desktop load, and the
  200 ms render-ahead needs no more than that.
- **When the background classes are real-time.** Only when the callback is
  FIFO/RR (`Backend::callbackPolicy()`; JACK reads its client thread). A
  normal callback (the dummy backend, a non-RT JACK server) leaves them
  normal. `EngineOptions::background` forces `Normal` or `Fifo`
  (`backgroundPriority`) for benchmarks and tests.
- **How it is applied.** On the main thread after backend start, through
  `pthread_setschedparam`, verified by reading the policy back
  (`setThreadPolicy`, core-base `supple/base/thread.h`). JACK's and rtkit's
  thread APIs are not used here. It is all or nothing: the renderer and every
  helper stay normal, with a `warnings` entry naming the cause and the fix, when
  - no priority ≥ 1 remains;
  - `RLIMIT_RTTIME` is finite (a renderer catching up runs without blocking
    and would be killed);
  - the renderer plus its helpers would leave fewer than two of the process's
    allowed CPUs to the rest of the system;
  - or any thread refuses.

  Off Linux the policy asked for is the platform's High (no priority levels
  or rlimits; the CPU rule still applies). On Windows the main thread sets it
  as above. macOS takes a thread's QoS only from the thread itself, so there
  the pool's helpers and the renderer set it on themselves as they start,
  all or none through one startup barrier (as with a device's own policy):
  if any of them refuses, every one goes back to normal, and the start waits
  for that before it reads back what each holds.
- **Lifetime.** Threads end at `stop()`, so nothing is restored. CPU affinity
  is never changed: threads keep the inherited mask.
- **Bridges.** `host::setBridgeAudioPolicy` carries the applied background
  policy to bridged instances while the engine runs (normal otherwise), and
  `host::setLiveBridgeAudioPolicy` the callback's policy for live calls
  (ADR-0081): each bridge audio thread takes the class each command asks
  for (§5). `engine.status.bridge_threads_refused` lists the isolated FX
  whose bridge thread was refused it.
- **Reporting.** `Engine::scheduling()` records each class's applied policy
  at start (None for a class without threads, and for all while stopped).
  `engine.start` and `engine.status` report it as `scheduling`: `callback`,
  `live`, `renderer` and `background` objects, each with `policy` (`none`,
  `normal`, `fifo`, `rr`, `high`) and `priority` (0..99).

**Thread names.** Every audio thread the host creates names itself when it
starts (`nameThread`, core-base `supple/base/thread.h`; Linux keeps 15 bytes),
so `ps -L`, `top -H`, debuggers and profilers tell them apart. The names carry
the role, never the product: `audio-render` (renderer), `audio-bg-N`
(background/offline pool helper N, from 1), `audio-live-N` (live helper N),
`audio-bridge` (a bridge process's per-instance audio thread), `audio-dummy`
(the dummy backend's thread), `audio-recorder` (recording writer),
`audio-offline` (offline render thread, engine and sandbox) and `audio-paced`
(the benchmark's paced device). The JACK callback thread belongs to the JACK
library (PipeWire's `data-loop.N`) and keeps its name. `pw-top` lists graph
nodes, not threads; it shows the JACK client name.

**Live pool (ADR-0024).** `LiveWorkers` shares the graph/ready-queue
representation, but never calls the blocking background executor. Each segment
publishes its immutable transport, device inputs and frontier buffers, opens
the job, then advances selected helpers' private 32-bit sequences and calls
`rt::wake`. This core-base API issues only `FUTEX_WAKE_PRIVATE` under the scoped
RTSan exception; its blocking `rt::wait` companion belongs only to idle helpers
and rechecks the acquire-loaded word after spurious wakes/EINTR. Both require a
live, naturally aligned lock-free 32-bit atomic and one waiter per word. C++
atomics publish the job; futex calls convey no memory order.

The plain job pointer/function/context are published while closed, after the
previous join has excluded all readers. Opening is a seq_cst store. Each waking
helper increments active (seq_cst), then reads open (seq_cst): only if open may
it read job fields and help; otherwise it touches no job state. It decrements
active with release after all accesses. When all nodes finish, the callback
closes open (seq_cst) and spins on active (seq_cst) until zero. This store-load
handshake needs seq_cst on both sides: either the callback observes the claim
or the helper observes closed. A helper preempted before claiming does not
retain the job; an old wake may harmlessly claim the next open job.

The callback helps execute ready nodes and spins on atomics (CPU relax only)
while work or claimed departures remain. Helpers spin only within that job and
park between segments, with no post-cycle spin. The one exception
(ADR-0085): while a bridged call of the job blocks past its spin under the
live deadline (`host::LiveBridgeBlocked`, counted in the pool's
`host::LiveBridgeWaiters`, which `run` sets on the callback and each helper),
an idle helper parks on its own futex word, after setting its parked bit and
a fence, and only if the count is still nonzero. The call's end (decrement,
fence, read the bits) wakes every parked helper; a push wakes as many as it
pushed, best effort. No helper is parked past its job, since the job's last
node ends after its waits. In-process jobs never park: the count is read
once per idle spin and the parked bits once per push, relaxed. Participation uses the chain
cover, configured capacity and usable CPUs minus one; a chain runs directly
without queue/wake traffic. The callback joins a helper preempted after claiming,
including after completing DSP. No helper touches the job after its departure
acknowledgement. Only after join may the callback publish
meters/plug-in outputs, capture recording, reset/relocate, adopt a plan, or
acknowledge a newer generation. This extends existing plan reclamation and
ownership tokens to every helper. There is no late-output discard, timeout or
rerun: a late in-flight node makes its cycle late. A processing error still joins
all work before silencing/counting the failed segment; it never cancels in-flight
plug-in state. Summation order and numeric policy (§2.4) remain identical to
offline processing. Bridged stages run on the callback and on helpers alike:
`LiveWorkers::run` hands the callback's bridge deadline to every helper that
claims the job (ADR-0081).

`Backend::liveThreadCapacity()` and `prepareLiveThread(native_handle)` run on
main after backend start, before publishing the pool to callbacks. Unknown
backends refuse setup by default; normal/dummy backends explicitly accept
normal helpers. JACK reads the actual callback thread's policy/priority and
allowed CPU set through its optional client-thread API and pthread queries.
Live capacity is capped to CPUs common to callback and caller minus one.
SCHED_FIFO/RR callbacks require helpers at the same priority with SCHED_FIFO:
try `jack_acquire_real_time_scheduling`, then pthread scheduling within
RLIMIT_RTPRIO, and verify the result. Non-RT JACK uses normal helpers. Failed
setup/capacity disables the pool and retains serial processing with an
actionable warning. No affinity or system configuration is changed; externally
changing CPU affinity while running requires an engine restart. The backend
stops before helpers are destroyed. `Engine::liveHelpers()` reports enabled
capacity; `engine.start`/`engine.status` expose `live_helpers` and `warnings`
(including plan and scheduling warnings), zero/empty when stopped apart from
optional process-control failures described below. These are
capacity, not a count of participants in every segment. Automatic enables this
path; Audio processing threads Limit 1 disables it on next start.

### 2.3 The RT cycle

1. Pick up a new plan if published (acknowledge epoch).
2. Drain the model→RT command queue (transport, param events, meters on/off).
3. Read hardware inputs into HwInput buffers; feed record rings (and, for
   recorded masters the renderer runs, pull their record rings, ADR-0057).
4. Pull anticipative outputs from rings; fork/join the live DAG.
5. Sum into the master, run the master chain (live if anything is live, else
   the master is anticipative too and the RT thread only copies its ring
   output).
6. Write hardware outputs; publish meters (lossy rings) and state deltas
   (coalescing slots).

When driving a manual dummy backend, wait for `Engine::awaitPlan()` after
publishing a plan before counting its output-settling callbacks. This wait
ends after the renderer has adopted the plan and queued it for the callback.
`awaitRendered()` waits for audio in the transport epoch, not for the latest
plan; while stopped it can return immediately. Fast manual callbacks do not
pace the renderer like a device clock does. Both waits belong on the
test/model thread. Each is bounded by `awaitSeconds()`, 10 s unless a call
passes its own bound: tests raise it to 60 s, as they do the transport's wait
(in process with `setAwaitSeconds`, for a binary a test starts with
`SUPPLE_TEST_TRANSPORT_PATIENT=1`), so a starved renderer thread never fails
a readiness wait.

**Device outputs (ADR-0056).** The plan lists the masters with a
`hw_output` (tr_0 plays to 1/2 unless it says otherwise) as output taps.
Every executor writes all of the device's opened channels: each tap's main
pair into its channels (one channel: (L+R)/2), the first tap of a channel
copied and later ones added, every other channel silent; taps beyond the
device's channels are skipped and `engine.load` adds a warning naming them.
The anticipative root ring carries every device channel; frontier rings stay
stereo. Output masters are live whenever anything is live. The main master
and every output master are latency anchors: `outputLatency` is the largest
of their `latIn + latChain`, and each anchor's `timelineShift` is
`outputLatency − latChain`, so all of them reach the device together (with
only tr_0, its own latency as before). A key route from one output master
into another's path can pull the first earlier; the plan then warns with the
sample count. The metronome plays on outputs 1/2. Offline renders of any
master use that master's own shift and latency, so the device output of each
master equals its offline render at `outputLatency`.

**File audition** (ADR-0094). `Engine::audition(voice)` (main thread) plays
an `AuditionVoice` (decoded audio at the device's rate; `right` empty: mono)
from its first sample at the next cycle: the callback adds it to outputs 1/2
in `Engine::cycle`, after `cycleTransport` and before LTC (`runSync`) and the
hardware MIDI lead's delay, in both executors, stopped, playing or counting
in. It is outside the plan: no track, master, plug-in, Console 9 stage or
meter sees it, renders never run the callback and recordings never contain
it. A new voice or a stop (`audition(nullptr)`) fades what played over 5 ms
(`kAuditionFadeSeconds`; a third request inside a fade cuts the older fade).
A stopping engine adds nothing and `Engine::stop` drops the voice. Each
request is immutable with a serial, published through one atomic pointer;
the callback stores the smallest serial it may still read (`auditionKeep_`:
the request it took, the voice playing, the one fading) and `service()`
frees requests below it. `auditionState()` (any thread) gives the playing
voice's serial (0: none), its frame and the serial last taken; the end of a
voice notifies the main thread. `EngineApp::playAudition` reads the file's
start with `audio::readAudioHead` (two channels), up to
`AppOptions::auditionMaxFrames` (28,800,000) counted at the higher of the
file's and the device's rates, so neither the decoded file nor the voice is
longer (about 230 MB of stereo float; ten minutes at 48 kHz, 2.5 at 192 kHz;
`truncated` when the file is longer), and resamples it to the device's rate on its own worker
(`AuditionLoader`: one replaceable request, cancelled by a newer one or
`stopAudition`, its result taken in `tick()`), then publishes the voice; a
device restart before then drops it. `auditionStatus()` is what the GUI
reads each frame (`playing` from `audition.play` until the end, `decoding`
before the first sound). `stopAudition` runs when the engine halts and when a
project is replaced.

**Meters**. Per strip, the peak and the mean square of the strip's signal
**after the fader and after the pan law, before the Console 9 encode**
(`sum × pan matrix × faderGain` — what a user expects at unity: a −6 dBFS
item at 0 dB reads its own peak times its strip's pan law at its position,
which is unity at the centre under the default 0 dB law, ADR-0066)
since the reader's last call; a master has no encode and measures its output,
the signal the device is given. Two SPSC rings of `MeterEntry` carry the
numbers, one per producer: the renderer thread publishes what it renders ahead
(after the block, collected from its nodes, never from worker tasks), the audio
callback publishes what it plays — the live nodes, or the whole graph when
anticipation is off. An entry is **heard** once the callback's play position in
that epoch has passed the end of the block it covers, minus the device's
reported output latency; entries of an older epoch (a seek or a pause bumped
it) are dropped, and while the transport is stopped everything already
processed counts as heard (monitored input). `EngineApp::meters()` drains the
rings and returns one reading per strip of the newest plan — zeros for a strip
with no heard audio; decay and peak hold belong to the GUI. Each entry carries
its slot generation from the publishing plan; entries from a retired allocation
cannot contaminate a newer track even when loads outrun plan adoption.
A full ring drops
the entry and counts it (`engine.status.meter_drops`, per producer). Offline
renders do not meter at all (no bank: zero cost), and the publishers never
touch the signal, so renders stay bit-identical.

Everything in this cycle is `SUPPLE_RT` and runs under rtsan in the `rtsan`
preset's engine tests (with the dummy backend).

### 2.4 Floating-point environment and first touch (ADR-0025)

During the initial platform port (ADR-0034), the engine remains gated on
macOS and Windows. `DenormalGuard` currently saves/restores SSE2 MXCSR;
its state is unused on ARM64 until the floating-point environment port step.
Portable DSP tests do not establish ARM64 RT/offline environment equivalence.

`detail::processNode` scopes `dsp::DenormalGuard` around each node: its
input sums, delays, Console 9 stages, faders, oversampling, meters and every
in-process plug-in `process()` run with FTZ and DAZ added to the executing
thread's MXCSR, which is restored when the node returns. Every executor runs
nodes only through `processNode`: the device callback (serial or with live
helpers), the live helpers, the anticipative renderer, background workers and
offline renders. Each node therefore computes under the same environment in
real time and offline, whatever thread runs it, and the device callback's
thread (owned by JACK/PipeWire) leaves every node with its own MXCSR. A
plug-in that changes MXCSR in `process()` affects at most the rest of its node.
The bridge applies the same flags around each bridged `process()` (§5), the
offline stem decoder around its decode (§3), and the plug-in sandbox around
each render block.

Plug-in lifecycle calls (`start/stop_processing`, `reset`, parameter flush
outside `process()`), transport computation, copies, the metronome and
offline post-processing (normalization, limiter, dither, encoding) keep the
thread's environment in every executor. A value that underflows inside a node
becomes zero; normal-range results are unchanged. The guard uses SSE2 FTZ/DAZ on x86 and FPCR FZ on arm64 (ADR-0035);
other architectures keep their default environment. The full control register is
restored, including when a plug-in changes it. Cross-platform render bit identity
is not required; per-platform executor equivalence remains required.

Audio sample buffers (node and stage buffers, delays, the render-ahead and
frontier rings) are zero-filled on the main thread when a plan or ring is
built, which commits their pages before any callback uses them. A recycled
ring (§1.6) is not zero-filled again; its pages were committed when it was
first built. Owned audio
threads (live helpers, background workers, including an offline render's, the
renderer and the bridge's audio threads) call `rt::prefaultStack()` once
before their first job: it touches the 128 KiB of stack below the caller and
does nothing when less than 512 KiB remains. The device callback's thread is
not ours and is left alone. Reserved event and voice capacity is committed on
first use, and no plug-in code runs early to warm it.

## 3. Offline rendering

- Runs a **separate plan instance** with its **own plugin instances**
  (created from the version's state/patches), so rendering never disturbs
  playback and A/B renders of two branches can run side by side.
- **Bridged plug-ins (ADR-0090).** Once it has made its instances, a
  render gives each bridged one a call deadline of its own,
  `OfflineSettings::bridgeDeadlineMs` (`kOfflineBridgeDeadlineMs`, 60 s;
  lifecycle commands 5/2 of it plus 2 s), in place of the live paths'
  process-wide 2 s, and restores that when it returns, whatever it
  returns. Its control requests (activation, the readouts below) get at
  least the lifecycle value. An instance that fails while the render runs
  (starting, processing or stopping: missed deadline, crash, exit) stops
  the block loop and fails the render with `engine.fx_failed`: message
  naming the plug-in, its fx id and the reason (and any others that
  failed with it), path `/fx/<id>`, a hint to render again or bypass or
  replace the FX. No caller then has audio to write. An instance already
  failed when the render begins keeps its behaviour: a restart failure is
  a warned, compensated pass-through; a failed bridged instance refuses to
  start (`engine.start_failed`).
- **Stop and cancel.** The block loop checks `OfflineSettings::cancelFlag`
  and `stopToken` between blocks. From the same point as the deadline, a
  watch thread also checks them every 10 ms while the render has bridged
  instances; on either it kills the render's own bridges
  (`host::Instance::abandonBridge`), so a render waiting in a bridged call
  (activation, reset, flush, start, a block, stop, a readout) returns at
  once. Every caller gives a render its own instance pool, so no other
  user shares those bridges. A render stopped or cancelled either way
  returns `job.cancelled`, ahead of any failure of the instances it killed:
  a render with a failure first ends the watch (a call ended by its kill can
  return before the watch has recorded the kill), and a stop or cancel asked
  for by the time the outcome is decided is the outcome.
  `setRenderWatchTestHooks` (internal `src/render_watch.h`, tests only, set
  while no render runs) holds the watch after its kills until the render
  ends it, which orders that race the same way every time.
  Instance creation, before that point, keeps its control-plane timeouts
  (20 s for `Create`).
- Once a render has succeeded, each plugin's read-only parameters (meters,
  readouts) are read from its instance (`OfflineResult::meters`, by FX id);
  a failed or cancelled render reads none. A `render.mix` keeps them on the
  main thread for `fx.meters {from: "render"}` until the next one. Other
  renders keep none.
- Fixed block size (default 512; setting), no anticipative split, all nodes
  in topological order, optionally parallel across independent nodes —
  results must be **bit-identical for any worker count** (fixed summation
  order, §1.2) and across repeated runs (acceptance test).
- Real-time/offline match: for sessions of stock plugins (block-size
  invariant by their own acceptance tests), a real-time capture through the
  dummy backend at any device block size nulls against the offline render to
  −120 dBFS (acceptance test). Third-party plugins that are not block-size
  invariant are documented exceptions.
- Render jobs (`render.*`): mix, stems (per track post-fader through the
  master-less path, or through master using an exclusive solo — option), regions,
  the render matrix, selected items; file naming per session-model §7.3; a job
  queue; results reported as files + analysis summaries. `render.mix`,
  `render.stems`, `render.regions` and `render.dry-run` share one
  implementation and one argument set (ADR-0055).
- **A job that stops early** (`render.mix`, `render.stems`,
  `render.regions` and `render.queue.run` with `job: true`) keeps the files
  it has written, and its snapshot lists them in `result` beside `error`
  (failed) or alone (cancelled): `files` holds the entry of every task it
  finished, as a finished job's `files` does (path, outputs, render id,
  …; across all queued renders for `render.queue.run`), and
  `unfinished_task_files` the paths a task that did not finish had already
  written (an output or a loudness report before a later one failed). With
  nothing written there is no `result`. A call without `job` returns only
  the error.
- **Settings** (ADR-0055, session-model §1.10). Every member of
  `project.render` is also a command argument of the same name. The legacy
  arguments `format`, `bit_depth` and `secondary` remain:
  - `format` and `bit_depth` override `outputs[0]`'s members.
  - `secondary` replaces `outputs[1]`, or with `"none"` removes it.
  - Giving `outputs` together with any of them is `render.args`.

  A field resolves from the arguments, then `project.render`, then the user
  default, then the built-in default. Object fields are replaced whole.
  Arguments are never written back to the project. Contradictory settings
  are reported as `render.args` with the offending path and a hint:
  - `seamless_loop` without a tail, or with `trim`, or with `handles_s` on
    regions
  - `scope: master` on stems together with a ceiling
  - a bit depth or sample rate the output's format does not support
    (32 bits outside WAV, a bit depth on MP3 or Vorbis, an MP3 rate other
    than the MPEG rates 8000–48000, Vorbis above 192 kHz)

  Settings that only some sources use are no contradiction elsewhere, so
  File → Mix still works when the dialog was set up for stems: `handles_s`
  and `matrix` apply to regions, `stems` and `scope: master` to stems (any
  other file normalizes on its own; the mix is the master), and
  `channels: "track"` gives each file its source's own width (a 1-channel
  track's stem is mono; the mix and regions are stereo). The error's message
  says when the offending member came from the project or the user default.
- **Chain**, in this order, per file:
  1. Offline render of the range plus the tail, at the project rate.
  2. Seamless-loop fold: the tail's samples are summed onto the start,
     wrapping again if the tail is longer than the range.
  3. Resample to `sample_rate` (the audio-file polyphase resampler).
  4. Mono fold, (L+R)/2.
  5. Normalization: measure with core-analysis, then one gain, clamped to
     ≤ 0 dB with `only_reduce`. With `scope: master`, the gain comes from a
     master-mix render of the same range through steps 1–4.
  6. True-peak limit (look-ahead gain envelope, 5 ms attack, 50 ms release,
     4× or 2× oversampled detection, then a final trim), whenever
     `limit_true_peak_db` is set.
  7. Trim of silence at either end.
  8. For each output:
     - resample to the output's own rate;
     - quantize with seeded dither (`tpdf`, or `shaped`: TPDF with fixed
       error-feedback noise shaping) at 16 or 24 bits;
     - encode: WAV (PCM or float, RF64 beyond 4 GiB), AIFF, FLAC (the
       original audio-file encoders, which quantize alike), MP3 (LAME 3.100
       in `libs/audio-codecs`, joint stereo, algorithm quality 2, with a
       LAME/Xing header for gapless playback) or Ogg Vorbis (libvorbis
       1.3.7, its stream serial number taken from the output's seed).

  Metadata goes where each format holds it: WAV a LIST INFO chunk, `bext`
  and cue points with labels (a region as labelled text with its length);
  AIFF NAME, AUTH, ANNO and MARK chunks; FLAC and Vorbis comments and a
  front-cover PICTURE (Vorbis: METADATA_BLOCK_PICTURE); MP3 an ID3v2.3 tag
  (TIT2, TPE1, TALB, TRCK, TYER, TCON, COMM, TSRC, TSSE and APIC). The
  software field (ISFT, ENCODER, TSSE) and the bext originator are the
  product name. RF64 files carry no metadata chunks. The bext time reference
  is the file's start on the timeline in samples at the output's rate; its
  loudness fields are measured on the file's audio; its origination date is
  `metadata.date` when it is a full date, otherwise empty; nothing reads the
  clock. Shaped dither feeds the quantization error back through the fixed
  three-tap filter (1.623, −0.982, 0.109) before the TPDF dither.

  Each output gets its own dither seed derived from the project seed and its
  index. No step reads the wall clock, and the same session and settings
  give byte-identical files.
- **Checks.** When `normalize` targets a LUFS mode or `limit_true_peak_db` is
  set, each output is measured as written, with MP3 decoded through dr_mp3
  and Vorbis through vorbisfile. Its entry carries `checks: {loudness?:
  {mode, target, measured, ok}, true_peak?: {ceiling, measured, ok}}`.
  - Loudness passes within ±1 LU of the target, or at or below target + 1 LU
    with `only_reduce`.
  - True peak passes at or below the ceiling + 0.1 dB, allowing for lossy
    coding.
  - `report: true` writes `<file>.loudness.txt` beside each output, with no
    timestamps. It lists:
    - integrated loudness, LRA and max short-term and momentary loudness;
    - true and sample peak;
    - duration, format and the checks.
- **Results.** Each file entry has:
  - `path`: the first output;
  - `secondary_path`: the second output, if any;
  - `outputs`: `[{path, format, bit_depth?, sample_rate, checks?}]`;
  - `branch` and `version`;
  - `start_beat` and `seconds`, after any trim;
  - `analysis` and `warnings`.

  `skipped: [{track?, region?, reason}]` lists files left out by
  `skip_silent`, `if_exists: skip` or, for stems with no `tracks` named, a
  channel that can produce no audio (`reason: "no_audio"`, with a `detail`; a
  track named in `tracks`, or in `stems.tracks`, is always rendered). That
  is a channel that has a rack, none of whose Out modules (the only way audio
  leaves a rack; Mod Out carries control and Notes Out plays an instrument
  elsewhere) has an audio output cabled into it, and that nothing else feeds: no
  items, FX, print, children, MIDI input, hardware input or send into it, and
  no rack Out with audio patched to it (`modulate`'s Modulators channel, whose
  Out `rack.add` made and nothing is patched to). A cable from a source this
  build does not know counts as audio. It leaves the file counter alone. `render.dry-run` lists each file with
  `path` (where `if_exists` sends it), `exists` (its destination is already
  there), `skip: true` when `if_exists: skip` leaves it out, `seconds` and
  `outputs: [{path, format}]`; a skipped no-audio channel is `{track, reason,
  skip: true}`. With no `operation` it plans the settings'
  `source`. `render.mix` answers with its file's entry at the top level
  (plus `peak_dbfs`, `render`, `output_latency` and `skipped`), as before.
  The mix renders the `masters` setting, a file each in that order
  (ADR-0056; `master` for one call; absent: tr_0 with `{track}` empty, as
  before); with several, the first file is on top and `files` lists all, and
  a pattern without `{track}` gets `-{track}`. A listed id that is not a
  master is `render.masters`.
  A later output with no `suffix` whose extension repeats an earlier one's
  is named with its bit depth, bit rate or quality (`mix.16.wav`,
  `mix.128k.mp3`, `mix.q6.ogg`); two outputs that still collide are
  refused (`render.path`).
- **Queue.** `render.queue.add` pins the effective settings (with a loop
  range turned into custom bounds) as well as the version, so a later
  default, preset or loop does not change what it renders.
- **Settings commands.**
  - `render.settings` (query): `{effective, project, default, from: {field:
    "args"|"project"|"default"|"built-in"}}`; accepts the render arguments as
    overrides.
  - `render.preset.list` (query): factory and user presets.
  - `render.preset.save {name | default: true, settings?}` (action): saves
    the given settings, or the effective settings, without `stems.tracks`,
    `matrix` and custom bounds.
  - `render.preset.load {name}` (edit): replaces `project.render`.
  - `render.preset.delete {name | default: true}` (action). Factory presets
    cannot be overwritten or deleted.

  Files are `<data>/<slug>/render-presets/<name>.json` (the user default is
  `default.json`), canonical `supple.render-preset/1.0`. Under `--workdir`
  they are kept in `<workdir>/render-presets`.
- `branch.create` accepts `version` (instead of `from`): the new branch starts
  at that version, which may be in the current branch's history or, for a
  render of another branch, in that branch's; it takes the ID counters of the
  branch it starts from.
- Stems of channels and busses (isolated or through the master) and freeze
  captures leave every master's own items out; a master's mix, including a
  stem whose track is a master, keeps them (ADR-0057).
- A post-fader stem of a channel or buss is decoded outside the graph, under
  the same floating-point environment as the graph's nodes (§2.4).
- An isolated stem or a single-track render (`render.stems`, `analyze.track`)
  keeps the target and what feeds it. It also keeps what modulates
  any of those: a modulation route into a kept node keeps its source rack,
  the rack's home channel, and whatever feeds them (ADR-0096). Those are
  dependencies and are not heard, since the render taps the target. So a
  stem carries the modulation exactly as the mix does. Routes whose rack or
  target is not kept are left out.
- Through-master stems use playback's solo signal-path rules: retain the
  selected strip, its upstream contributors and its path through parents and
  ordinary sends to the master. Ignore other pre-existing solos and solo-defeat;
  preserve explicit mutes. Sidechain keys retain playback's dedicated taps:
  key sources continue driving FX even when their audible output is solo-
  silenced; post-fader keys still follow the source fader, pan and mute.
- Normalization rejects non-finite source samples, derived gains, or resulting
  float samples with `render.normalize` and a repair hint before applying gain.
  This is a representability check, with no additional dB target range policy.

## 4. Media, recording, device handling

- **Media cache**: decoded and resampled audio (Signalsmith-quality
  resampler or the audio-file polyphase resampler), in memory and keyed by
  media identity, engine rate and playback variant (implementation notes below).
  Disk backing under `$XDG_CACHE_HOME/<slug>/media/` and streaming readers
  prefetching on I/O threads into per-item rings (default 1200 ms like REAPER's
  media buffer) remain planned.
- **Peaks**: min/max pyramids built on I/O threads, cached by content hash,
  never in project diffs.
- **Content hash**: `MediaSource.hash` is `"sha256:<hex>"` of a file's bytes —
  read with the file when it is imported (`item.add`, media import). Recording
  stop drains capture and closes the WAV (final header sizes only); it commits
  takes immediately as one "record" undo step with empty hashes. An existing
  job worker rereads the closed file, then a worker finalizes its sidecar at
  the current location (including a first save moving scratch media). The
  model-thread completion fills matching media in every stored version without
  changing version IDs, object revisions or undo/redo history; already leased
  snapshots stay immutable. Transactions opened before completion retain the
  hash on commit. If the originating branch has undone/deleted the take, or the
  project was replaced, the update is dropped. A save while the job is pending
  writes a valid empty hash. A first save closes the worker's reader of the scratch file while it
  removes the file (a file that is open cannot be removed on Windows) and the worker reads it
  again from the new place, so the hash still arrives; the decode worker that prepares media for
  the plan is stopped for the move and asked again after it, and so is a file audition's decode.
  A reader outside the engine registers with `EngineApp::addScratchReader`: the save calls it,
  after the project points at the copies and before it removes the files, with the scratch media
  folder; it returns once it has let go of every file there and gives back what the save calls
  when it is done (the GUI's peak worker: `PeakService::holdUnder`). Readers on other threads
  (offline render, freeze, consolidate and analysis jobs) are not held: each has a file open only
  while it decodes it. Hash/read/sidecar failures leave it empty and emit
  `bus.post_failed` with a structured error. Adding the same bytes again reuses
  a media source whose hash is already known. Metadata for an import comes from the container's header
  (`audio::inspect`), never a decode: adding one file to many items costs one
  header read at a time. Projects saved before the hash existed keep their empty
  hashes (the peak cache falls back to path + size + mtime); adding one of their
  files again reuses its source — the path and the header it was added from
  decide — and fills the hash with the bytes now on disk, so such a project
  converges instead of growing a second source for one file. A file is hashed
  the same way when it is recorded.
- **Recording shutdown**: `Engine::stop()` joins the audio executors and writer,
  retaining finalized audio and MIDI takes for one subsequent `stopRecording()`
  call. `EngineApp::stopEngine()` (including device restart and destruction)
  retrieves and commits them as one record undo step after the engine is gone;
  commit failures emit structured `record.error`. Headless MCP EOF/error and
  socket-loop shutdown explicitly stop the engine while the session and bus
  still exist. This does not save a project: clients must stop recording and
  save before disconnecting to retain MIDI/session edits across process exit.
  Audio WAVs and placement sidecars remain recoverable, and so do MIDI takes
  through their journals (ADR-0080). Forced termination cannot execute this
  shutdown path.
- **Recording admission**: an armed track with `record.mode: output` fails
  `transport.record` with `record.output_unsupported`, its `/tracks/<id>/record/mode`
  path, and a hint to select input mode. This check runs before any recording
  files are created, including files for other armed tracks. Existing projects
  with output-mode settings still load; output recording is not supported yet.
- **Recording**: RT thread writes input frames into large preallocated SPSC
  rings (≥ 10 s); an I/O thread writes them to WAV/RF64 files incrementally,
  rewriting the header every 5 s so a crash leaves a valid file up to the last
  flush. Targets reserve their sanitized track/take stems with ASCII case folding
  before any files are opened, so simultaneous names cannot collide on
  case-insensitive filesystems. Existing WAV, sidecar and audio/MIDI journal entries
  also reserve their stems. A sidecar per file records its track and start;
  `project.recover` repairs the header to cover everything on disk and places
  takes a crash left out, clipping placement to trusted journal/sidecar progress
  when present (ADR-0014/0082). Repair checks both RIFF and data byte
  counts against the whole frames physically present, including a torn size
  whose rounded-down frame count already matches; partial trailing frames are
  not included in either repaired size. Punch in/out and loop recording (one take per pass; a pass over an existing item is a take of
  it, heard where it was recorded through a `take.comp` made in the commit's transaction,
  ADR-0103: `record.commit` carries `stack`); MIDI recording
  into MIDI takes, each journaled for crash recovery (below). Audio ring overflow becomes equal-length silence at its file
  position and is still reported; loop run positions remain in file-time units.
  Disk and periodic header errors stop only the failing target and retain that
  target's first error. The loop run table holds the initial run plus 4096 wraps;
  when full, capture stops adding input and returns retained passes with a
  structured `recording.loop_runs_full` warning for the commit. Sidecars require
  `supple.recording/1.0`, `1.1` or `1.2`, a track id, a positive finite sample rate, and complete,
  ordered loop runs beginning at file frame zero, with the loop range they were
  recorded in, or `[0, 0]` when seeks split the runs while no loop was active
  (an inactive loop range is never recorded); invalid sidecars return a
  structured error and must be skipped during recovery. Placement sidecars
  survive pending hash jobs (including finalized loop runs at stop); the
  background finalization adds an optional `hash` without changing their
  recovery contract.
- **Streaming RF64** (ADR-0082): new audio takes reserve a 28-byte JUNK payload
  in their 80-byte RIFF header. A take crossing the 32-bit size limit promotes
  that space to ds64 and becomes RF64 in place, without moving samples or
  stopping capture. Recovery reads/repairs old 44-byte recordings and new
  RIFF/RF64 recordings, including bytewise-torn promotion FourCCs in the
  otherwise unchanged fixed layout. Decoding, metadata
  inspection and media import accept RF64. Tests inject a small size threshold
  or use sparse files, without writing a 4 GiB audio payload.
- **Authoritative punch placement** (round-18 correction): audio/MIDI journal
  version 1.1 records `punch: {enabled, start, end}` in the header, in the take's
  sample-rate frames. Coordinates are nonnegative and at most 2^48; an enabled
  range increases. Mid-take punch edits retain ADR-0015's commit-time behavior:
  the writer snapshots the latest model setting off RT and includes a changed
  punch in the same complete progress record as captured/physical frames.
  A torn or invalid progress line advances neither frames nor punch. Normal
  stop takes the final snapshot; recovery uses the last trusted one, never
  the recovering app's transport setting. Version 1.0 journals load with punch
  disabled. Final sidecars 1.2 persist the same punch and trusted frame bound;
  sidecars 1.0/1.1 remain readable with punch disabled. Hash completion and
  retirement preserve it. Mixed recovered punches are logged as contiguous
  `record.commit` groups within one undo transaction, using its existing punch
  arguments; history replay therefore retains each take's placement.
  `RecordedPunch` carries `{enabled, start, end}` in recording-rate samples.
  `Engine::startRecording(targets, punch = {})` starts with disabled punch by
  default; `setRecordingPunch(punch)` is model-thread-only. The recorder's
  model/writer mutex protects only the small punch snapshot, never file work;
  the callback never accesses it. `RecordedTake::punch` preserves the final
  snapshot for deferred commit. Initial sidecars 1.2 omit the trusted frame
  field until finalization; failed finalization therefore cannot retire a journal; the journal supplies live progress. No other non-model
  placement input is omitted: start/rate, latency-adjusted runs, initial loop,
  physical frame extent and MIDI input clocks are already persisted.
- **Audio placement journals** (ADR-0082): each WAV owns an exclusively
  created `<stem>.audio.jsonl`, schema `supple.audio-recording/1.1` (legacy 1.0 still loads). The shared
  MIDI journal reader/writer handles header, append, flush/sync and torn tails.
  Its header has `track`, `sample_rate`, `start_sample`, `latency`, `channels`
  and the initial `loop` and `punch`. Runs use the same captured-frame clock as MIDI;
  progress is `{"captured": n, "frames": f}`, where `f` counts complete WAV
  frames actually written (including rendered-input padding/trim). The writer
  flushes audio before progress, and syncs data/header before the journal at
  each five-second checkpoint. Audio journal failure stops that target and
  reports `record.error`; healthy targets continue. Recovery lists the WAV,
  clips placement to min(physical frames, last complete journal progress),
  and uses stop's initial start/latency/run rule. Extra WAV frames are kept,
  never guessed into an unknown placement. Loop-range changes retain the
  initial-loop limitation of ADR-0080. Before journal retirement, a final
  `supple.recording/1.2` sidecar preserves the trusted `frames` bound and punch;
  legacy 1.0/1.1 sidecars still load. Hash completion preserves the bound. Save/autosave
  checkpoint the stopped audio before publishing and retire owned journals
  only after success; Save As retires its new copies, never another project's.
`RecordedTake::journal` is the owned audio placement journal path (empty if
exclusive creation failed); it is not project media and does not enter
`record.commit`. Recovery reconstructs it from the checked WAV companion.
The final sidecar 1.1/1.2 `frames` is a nonnegative clock bounded by 2^48, and
clips recovery without truncating the physical WAV.

- **MIDI take journals** (ADR-0080): each MIDI target writes
  `<media>/rec/<Track>-<n>.midi.jsonl` (created exclusively, never through a
  link; a name any entry has, a dangling link too, is not free, and a take
  whose journal could not be created names none, so nothing already there
  is ever deleted as its journal), JSON Lines with schema `supple.midi-recording/1.1` (legacy 1.0 still loads): a header object
  (`track`, `sample_rate`, `start_sample`, `latency`, `device`, `routed`,
  `shift`, `loop`, `punch`), then events `[at, status, data1, data2]` (plus `route`,
  `voice` for Note Out voices; `at` in captured frames), `{"run": [captured
  frame, timeline]}` per loop wrap or seek, and `{"captured": n}`. The audio
  thread only pushes to the MIDI rings and stores how far capture got (one
  atomic store); the writer thread appends what it drains every 20 ms,
  flushes to the OS each pass and fsyncs every 5 s. Loss window: about 20 ms
  on a program crash (more only if the writer fell behind, up to the
  4096-event ring), 5 s on a system crash. Readers drop a torn last line and
  everything from the first line that does not read, or that holds what the
  writer cannot write: an event that is not a channel message (status
  0x80..0xEF, 7-bit data), a device event in a take with no device, a Note
  Out event in one that is not routed, a voice outside 0..2^31-1, or a
  clock (`at`, a run, `captured`) beyond 2^48 frames. A header whose clocks
  are beyond that (or `latency` negative, or a loop that is neither `[0, 0]`
  nor increasing), or that records neither a device nor Note Out, makes the
  file not a journal (`record.journal`). The bound keeps placement's
  arithmetic inside 64 bits. Stop and recovery
  place a take with the same function (`detail::placeMidi`). A journal that
  cannot be written gives the committed take a `record.journal` warning and
  does not stop the recording. The app deletes a committed take's journal
  once a save or autosave is written, where the save put it: a first save
  moves it with the media, and a save as into another folder deletes the
  copy it made there (an identical file already there is not its copy and
  stays; one a failed save as made is, and the retry deletes it, whatever
  spelling of the same file it is retried under: copies are known by file
  identity as the file system reports it, not by spelling, since a path
  through a link followed by `..` is not the path with both removed; a
  path where no file is matches no copy), leaving
  the old folder's for the old project, whose file does not hold the take. A save as into another folder waits for a running
  recording to stop (`project.recording`). Replacing a saved project without saving leaves its journals, offered on
  its next open; replacing an unsaved one without saving discards them with
  its folder (ADR-0078).
- **Recording a rack Out** (rack.md §6): an armed channel whose selected
  `record.input` is an Out module records its stereo input, after input FX
  and before its own items, inserts, fader and Console 9 encode. Arming makes
  the channel live, even with monitoring off; a non-live source rack reaches
  it through the existing audio frontier. The rack input remains audible as
  before, including when stopped or playing without recording. Armed input
  FX process this input, independently of device inputs being available.
  `RecordTarget::rackInput` selects the channel's prepared pre-FX buffer;
  `startRecording` resolves `outputShift` to its input timeline shift plus
  input-FX latency, or returns `record.rack_input` if no armed rack input
  exists in the loaded plan. The existing recorder aligns rendered input
  against its capture clock, keeps stereo float32 samples unchanged, and
  applies its normal padding, loop runs, punch, takes, sidecars, recovery
  and undo rules. Input-FX warm-up is dropped and an unavailable final tail
  is padded, just as for device input FX. Without input FX, its recorded
  musical span is bit-identical to the offline rack output.
- **Recording MIDI sends** (rack.md §6): an armed `midi_overdub` or
  `midi_replace` channel records every MIDI send or Note Out route delivered to it,
  after channel filtering/remapping, mute and compensation. Its selected
  MIDI input device may record alongside those routes. An instrument and a
  device record-input route are optional. Arming makes the destination and
  its MIDI sources live, including with monitoring off. Each node has
  preallocated storage of delivered events; the callback copies it into
  the existing per-track MIDI recording ring. A missing instrument still
  consumes the routes and records them.
  `RecordTarget::Midi::routed` enables these events; `startRecording` sets
  its `shift` from the destination node's timeline shift, or refuses with
  `record.midi_route` if the target is not live in the loaded plan. The
  internal events use processing position minus that shift; device
  events retain their own round-trip correction. A mixed take spans both
  clocks, and held notes end at their source's corrected stop. Stable route/voice
  addresses travel through `RecordedMidiEvent` and MIDI note pairing,
  preserving overlapping equal-pitch voices separately from device notes.
  These addresses are recording metadata only; saved MIDI sources retain
  their existing format. The shared MIDI commit path applies loop takes,
  punch, replace, overdub and one undo step.
- **Recording a master** (ADR-0057): an armed master records its output
  before its own items (`RecordTarget::output`; two channels). Every armed
  master, and every master still recording, has a record tap in the plan.
  Where the callback runs the node (the in-callback engine, or a live master
  in a split graph) the recorder takes the node's buffer; where the renderer
  runs it, the renderer copies it each block into the plan's record ring
  (epoch- and stream-tagged, published before that block's root or frontier
  chunk) and the callback pulls the frames it plays, recording or not, so the
  ring drains. Record rings count for the renderer's room, wake targets and
  the split graph's priming like frontier rings. A frame produced at
  processing position P belongs to timeline P − (S + chain) of that master;
  `startRecording` reads that shift from the loaded plan, and the recorder
  writes the master's frames (round trip − S − chain) frames later in its
  file (earlier when negative), so all files of a recording start at the
  same position and loop runs and punch apply unchanged. With no master items
  in the span the take is bit-identical to `render.mix` of that master.
  `Engine::setRecordingMasters` (before `load`) lists the masters recording
  and their punch range: their items are silent there (a per-sample range
  check on the items buffer) or, with punch off, left out of the plan.
  `transport.record` sets it and reloads before starting; stop clears it.
- **Backends** (ADR-0004): JACK (dlopen), dummy, offline in P4; buffer-size,
  sample-rate and device changes stop the engine cleanly, rebuild the plan,
  and restart; xruns are counted (backend callbacks + our own deadline
  misses) and emitted as `engine.xrun` events with context (block size,
  load, which node was slowest).

CLAP plugins without audio-ports-activation follow each block's key buffers
without restarting when a route changes. Their activation-time key flag does
not suppress a key connected later; a missing block key supplies silence.

## 5. Plugin isolation (`host/clap-host/bridge`)

**Default isolation (ADR-0081).** An fx's `isolation` names its process;
`inherit` is resolved when its instance is made
(`InstancePool::setDefaultIsolation`, from `EngineApp::setPluginIsolation`,
the GUI's preference): a stock plug-in (its file in `stockPluginDir()`) runs
in process, any other at the default (`in_process`, as shipped;
`shared_bridge`; `dedicated`). A bridged default with no bridge executable
set runs it in process and `engine.status.warnings` says so. An instance
keeps its process until it is made again (fx.restart, the plug-in replaced,
a project opened), so changing the default never reloads a running
plug-in. `fx.describe isolation_effective` says where it runs. Offline
renders, render snapshots and rack probes make their own instances: one on
`inherit` runs in process there, one set to `shared_bridge` or `dedicated`
runs bridged; isolation does not change the output.

**Load overrides (topic 7d, ADR-0081 note, ADR-0083).** How this run loads a
named plug-in, whatever the project says: `plugin.override {plugin, mode}`
with `mode` `normal`, `bridge` or `disable`, and `plugin.overrides` to list
them with the fx of each plug-in in the project. They exist so that a plug-in
a crash report points at can be loaded safely, by the person at the keyboard
or by an agent; nothing sets one on its own. The map is keyed `(format,
plug-in id)`, is owned by the app and changed on the main thread, lives as long
as the app (it survives `project.open` and `project.new`), and is in no project
file, no history step and no save: a project saved while one holds is the
project saved without it, byte for byte.
- **A value type, copied into every pool.** `LoadOverrides` is copied into the
  app's `InstancePool`, a render's pool, a render snapshot's, a rack probe's
  and a capture's when the pool is made, on the main thread, so no job thread
  reads the app's map. Every probe the app makes itself
  (`EngineApp::makeInstance`: `fx.describe`, `fx.param.set`, latency, plug-in
  modules' validation at open, MIDI learn, chain and preset loading) goes
  through the same override.
- **`disable`** is enforced where a plug-in is found (`findPlugin`, and
  `InstancePool::get` for pools that resolve from a catalogue copy): both
  answer `plugin.disabled`, so the existing missing-plug-in paths apply. A
  strip plays the pass-through placeholder with its stage (wet, bypass ramps,
  sidechain routing, latency 0) and the plan warns; a rack module of it is
  silent; an offline render equals the render with that plug-in missing. The
  project keeps the fx. `fx.describe` answers from the session and the override:
  `override: "disable"`, a description saying so, no `isolation_effective`, no
  parameter section. `engine.status.disabled_fx` lists the fx. `fx.add` of the
  plug-in and anything that would load it answers `plugin.disabled`.
- **`bridge`** makes every instance of the plug-in on a dedicated bridge,
  ahead of the fx's own isolation and the preference
  (`InstancePool::resolveIsolation`). It is refused with
  `plugin.override_unavailable` when no bridge executable was found (rather
  than the quiet in-process fallback of `inherit`, which is wrong for a safety
  choice) and for a stock plug-in (a stock plug-in is a Steppe fault: disable
  only). Rack plug-in modules go through the same pool and are bridged too;
  tests compare the render with the live run in both executors. Offline
  output is bit-identical to in process; live, a bridged plug-in runs under
  the ADR-0081 deadline, so on a loaded machine a late block is silent live
  and present offline. A per-call probe (`fx.param.set`, latency) of a
  plug-in whose fx has a saved patch or state borrows the cached bridged
  probe (one process per plug-in id, not one per call); with neither, a
  fresh bridged probe is made.
- **A changed override plays a fresh instance.** `plugin.override` drops the
  cached probe of that plug-in, retires its instances in the app's pool
  (`InstancePool::restartPlugin`, as `fx.restart` does: the old ones live until
  both executors have adopted a newer plan) and recompiles. Whatever state
  was never saved is lost and held notes end. Setting the mode a plug-in
  already has changes nothing.
- **Probes at load.** Opening a project validates each cable into a plug-in module with a probe
  restored from that fx's state. Under a bridge override a probe is a process, so a plug-in's fx
  with a state share its one cached scratch probe (each fx's state is restored onto it before its
  check) and those with none share one pristine probe of the load, instead of one process per fx.
  A fresh fx under a bridge override, one with neither a state nor a patch, has nothing to restore
  onto a shared probe, so a `setPlainParams` call (`fx.set` of plain parameters) on it, and a
  latency measurement made while it has no state, each make a bridge process of their own. The
  first `setPlainParams` saves the fx's state, and from then on it shares the cached probe, so
  this lasts for one or a few calls per fx.
- **Which plug-ins to protect.** `project.plugins`, `project.open`'s
  `project.suspect_plugin` warning, `crash.settle` and the clean-run settlement that
  decide it are in `docs/specs/diagnostics.md` "Safe-load in the engine".

The bridge is native on Linux, macOS and Windows (ADR-0037). Linux retains
memfd, SCM_RIGHTS, shared futexes and pidfd behavior. macOS uses unlinked
shm_open descriptors transferred over socketpair, shared address waits/wakes,
a pipe notification and kqueue child exit. Windows uses pagefile mappings
and request/done auto-reset events duplicated into the child, a manual-reset
outward notification, overlapped duplex control pipe and owned process/Job
handles. The existing background/offline bounded-wait exception and failure
semantics apply on each platform. Native editor hosting follows ADR-0040;
the DAW shell is integrated separately on port-gui.

POSIX protocol 24 / Windows protocol 25 carry that period as a duration
(`livePeriodNs`): in device frames, the bridge read it at an oversampled
plug-in's rate, 2 or 4 times the device's.
POSIX protocol 23 / Windows protocol 24 add the device period
(`livePeriod`) to each command, which sizes the macOS live class (ADR-0081).
POSIX protocol 22 / Windows protocol 23 add each command's timing to the
header: when the host posted it, when the bridge saw it and finished it, the
CPU the bridge thread ran on, its migrations, and the scheduling class it read
back after taking the requested one (ADR-0081 diagnostics, §5).
POSIX protocol 21 / Windows protocol 22 add each command's audio class and the
bridge's refusal flag to the header (ADR-0081); 20/21 added plug-in note and
MIDI output (ADR-0068). POSIX protocol 19 / Windows protocol 20 carry typed native editor parents
(ADR-0040), live editor scale changes (`EditorScale`) and Cocoa editor window
frames, with unchanged Header layout. Windows retains the v14
u64 recipient-owned resource after each frame prefix (all-ones means
none) and u64 request/done event handles after Activate's rate/min/max/size.
Release/acquire on the shared words publishes all arena data. Event waits
recheck before parking and after wake; stale signals never acknowledge a
command. The parent rejects resource-bearing replies without closing a
numeric handle supplied by untrusted child code, returning `bridge.malformed`. Native launch reuses the
hosting child helper's explicit control inheritance and null stdout, so
plugin logging cannot block the bridge on an undrained output pipe. The bridge
also retains native `--scan` compatibility; Windows takes Unicode arguments
and writes canonical scan replies through a private binary stdout copy.

Protocol 12 appends a main-thread `PluginStatus` query to the existing
control plane, with no shared-memory change. It forwards the generic
[plugin-status extension](plugin-status-extension.md) for shared and
dedicated instances. Protocol 13 appends `FactoryPatches` after it (no
payload, canonical JSON patch array string reply). The version subscription
now negotiates 17 on POSIX or 18 on Windows. Earlier operation numbers and the data plane stay unchanged.

EditorOpen carries `u8 kind, u64 parent, f64 scale`; see sandbox.md §1 for
kind values and validation. Windows bridge HWNDs embed across processes;
Cocoa Floating handles carry the owner's server NSWindow.windowNumber (zero
if unavailable), never an NSView pointer. The bridge owns its NSWindow/NSView,
uses normal level and orders just above the owner on open/reopen without
activating itself; the DAW activation hook repeats that ordering.
Native bridge main loops combine UI input, IPC readiness and plugin timer
deadlines. macOS uses AppKit/CFFileDescriptor; Windows uses native message
waits. Linux retains its poll/X11 path. Native editor lifecycle/transport
fixtures run headless; real display tests are opt-in. Native DAW shell
validation also requires port-gui integration.

`fx.patch.list {fx}` lists read-only factory patch documents as `patches`
in declaration order (empty if unsupported). `fx.patch.set` takes exactly
one of `patch` or factory `name`, using the existing validated replacement
path and undo transaction. Omitted parameters take defaults. Names are
case-sensitive; unknown names fail with `fx.unknown_factory_patch` at
`/name` and a hint listing available names; missing/both inputs fail with
`fx.patch_args`. Listing uses the live instance when present, otherwise a
scratch instance. It never changes the plugin or writes preset files.

A plugin's presets, for the FX window's preset menu and for agents:
`fx.preset.list {fx}` returns `presets`, the factory patches first (`source:
"factory"`, the plugin's order, the names of `fx.patch.list`) and then the
user's for that plugin id (`source: "user"`, sorted by name, with `path`).
User presets are `supple.preset/1.0` documents (sandbox.md) in
`AppOptions::presetDir`, by default `<config>/<slug>/presets`, one folder per
plugin id (characters other than letters, digits, `.`, `-` and `_` become
`_`); a file for another plugin id, or that does not decode, is skipped with
a `fx.preset_invalid` warning. `fx.preset.load {fx, name, source?}` is one
undo step: a factory preset goes through `fx.patch.set {name}`, a semantic
plugin's user preset replaces the patch (validated by the plugin), and a
plain plugin's user preset is its base64 state, checked on a scratch
instance before the model takes it (the instance's `params` are cleared).
`source` is needed only when a factory and a user preset share the name
(`fx.preset_ambiguous`); an unknown name fails with `fx.unknown_preset`.
`fx.preset.save {fx, name, replace?}` writes what the running instance has
(editor edits included; else the model's patch or state) and creates no
history; a factory preset's name is refused (`fx.preset_factory`) and an
existing user preset needs `replace: true` (`fx.preset_exists`). Names are
1 to 200 characters without `/ \ : * ? " < > |` or control characters and
do not start or end with a space or a dot (`fx.preset_name`).
`fx.preset.delete {fx, name}` removes a user preset's file; a factory name
fails with `fx.preset_factory`. Wet, bypass and other instance options are
not part of a preset.

`fx.describe` returns `status_available` in both summary and full modes.
When a live instance supports the extension it also returns `problems`,
an array of `code`, `message`, `scope`, optional source `line`, `hint` and
`silent` (audio silenced because of that problem). Queries never clear
observations. Without a live instance or extension, status is unavailable
and `problems` is omitted; a scratch instance cannot establish live health.
A failed status query (a crashed bridge, an invalid document) still
describes the plugin: status is unavailable and `problems` holds one entry
with scope `host` and the error's code and hint.
Descriptions and semantic schemas fall back to the live instance when a
scratch query fails, so a saved script that no longer compiles can still
be inspected; normal descriptions continue reflecting model edits.

Modes per instance (session-model §1.7): `in_process`, `shared_bridge` (one
bridge process for all instances in that mode), `dedicated` (one process per
instance).

- On Linux, the bridge executable (`<slug>-bridge`) hosts plugins with the same
  `clap-host` library. Control plane: a Unix socket speaking a small binary
  protocol for main-thread calls (activate, params, state, semantic
  extension, GUI create/set_parent with the host's X11 XID, timers).
  Data plane: a `memfd` shared-memory region per instance with input/output
  audio buffers, event arenas and a cycle header; wake via eventfd/futex
  through the bridge's shared futex wrapper (ADR-0013, distinct from the live
  pool's private `rt::wake`). The shared memory is laid out for the widths the
  host asks for; the Activate reply (protocol version 9) returns the widths
  the plug-in settled on (a VST3 may adapt its buses), and a mismatch
  re-activates once at those widths, so an isolated plug-in exposes the same
  layout as an in-process one.
- The bridge main loop polls its control socket, a coalesced main-thread
  request eventfd and every registered CLAP/VST3 descriptor, even without an
  open editor. Its timeout is the nearest plug-in timer deadline, rounded up
  to milliseconds (0 when due), or -1 when there are no timers. Core-base's
  `supple::MainThreadWake` translates atomic/`rt::wake` requests into pollable
  events (ADR-0029); there is no fixed idle cap.
- Protocol v11 adds a versioned process-level outward eventfd subscription
  and acknowledge/rearm (ADR-0031; sandbox spec §1). The child retains new
  notifications, idle outputs and editor requests and signals once until ACK;
  unchanged queries/repaint-only timers do not signal. A parked parent watcher
  forwards these publications and idle bridge death/HUP to the app's existing
  relay via `Instance::setMainThreadWake`, including remote instances. Main
  acknowledges before refreshing all shared-process members; per-instance pull
  operations drain retained proxy snapshots without IPC on unrelated app wakes;
  subscribed instances refresh only for deliveries or pending control changes.
  Remote editor polling is removed:
  remote instances have no parent timer deadline. Failure publications from
  audio use an internal core-base relay, never fd writes or subscriber locks.
- A bridge's idle audio thread waits indefinitely on its shared request futex.
  Control HUP/EOF and explicit shutdown publish quit, advance/wake that word,
  then join before unmapping; no 100 ms idle wait remains. Audio observes quit
  before accessing a command. Parent in-flight IPC failure deadlines are unchanged.
- The bridge's audio thread sets FTZ and DAZ around each `process()` and
  restores its MXCSR afterwards (§2.4), so a bridged plug-in computes as an
  in-process one does.
- Each instance's audio thread is named `audio-bridge`. Every command carries
  the class it runs in (protocol POSIX 24 / Windows 25; `audioClass` in the
  header): background, the policy in `audioPolicy`/`audioPriority` (the
  engine's background policy while an engine runs, normal otherwise; §2.2,
  ADR-0026), or live under a live deadline (ADR-0081): on Linux the
  callback's policy in `livePolicy`/`livePriority`, on macOS a
  time-constraint policy for the device period (`livePeriodNs`, a duration
  the engine sets with `host::setLiveBridgeAudioPolicy` from the device's
  period and rate while it runs, so an oversampled plug-in's rate does not
  scale it; without one, the command's block at the activation rate), read
  back after it is set, on Windows MMCSS "Pro Audio" at critical priority
  (the bridge links `avrt`). The thread changes class only when the
  request, period included, changes. The class read back on macOS is `high` for a time-constraint
  policy, else the QoS. A waiting renderer
  or callback therefore never waits behind a lower-priority thread. Applying
  it is best effort: a refusal leaves the thread as it was, and the answer's
  `classRefused` says so; `Instance::bridgeThreadRefused()` and
  `engine.status.bridge_threads_refused` report it.
- **Live deadline (ADR-0081).** `host::LiveBridgeDeadline` sets, for the
  calling thread, the steady-clock time (`host::steadyNanoseconds()`) by which
  bridged audio-thread calls must be answered. Under it a call spins briefly,
  then makes timed waits on the done word up to the deadline and nothing
  else: no `alive()` check, kill or reap. Unanswered, `process()` outputs
  silence (no output events) and returns true, and the request stays
  outstanding (a flag says so, not its number, which wraps through 0); every
  later call first waits for that answer (under its own deadline) before it
  touches the shared memory, and discards the late output. A block skipped
  meanwhile is silent and its audio is lost, but its events reach the
  plug-in at offset 0 of the next block it processes, in order and before
  that block's own: no note off, pedal or parameter change is lost. The
  block's own output is lost (`OutputEvents::lost`, also for a failed
  bridge), so the chain's MIDI bus ends what it held and resets the
  controllers it moved (ADR-0088, below). At most
  `RemoteInstance::kMaxCarried` (512) are carried; beyond that, or when they
  and the next block's would not fit the arena's 2048, the notes and MIDI are
  dropped, each parameter keeps its latest value, and, if anything but
  parameters went, the plug-in is reset before that block, so no note whose
  end it never heard hangs. A carry full of distinct parameters still takes
  a later value of one of them; a value for a 513th target is lost. A legal
  block (at most 2048 events) never loses one of its own to the carry: the
  carried parameters it sets again give way to its later values, as many of
  the rest as fit go before it, and the others wait for the next block. Stop, reset
  and flush asked for meanwhile are kept, once each, and sent first when the
  answer comes, in the order reset, stop, flush; a reset drops the carried
  notes, which came before it. A start not answered by the deadline returns
  false with `startUnanswered()` set; asked again, it returns the answer
  (no second start). Until then the callback only looks for that answer (an
  atomic load, no wait, so those cycles are not late), stop, reset and flush
  are kept, and the start is not judged by the late rule: it fails as hung
  once the lifecycle deadline (5/2 of the bridge deadline plus 2 s) has
  passed since it was posted (ADR-0101), a call off the callback that
  reaches it waiting only the rest of that deadline. A stop reaches such an instance too, and the bridge
  stops an instance still processing before it deactivates it. `host::bridgeLateCalls()` counts late calls
  process-wide. Consecutive late calls spanning 50 ms fail the instance with
  the hung reason; the audio thread only sets the flag, and the main thread
  kills the process when it services the instance. Without a deadline the
  background rules below apply unchanged.
  On Windows a timed wait waits on the done event together with a
  high-resolution waitable timer the client creates with the instance, so it
  ends by the deadline plus that timer's lateness, not a 15.6 ms timer tick
  later (measured before: 15.7 ms against a 2.67 ms deadline). Without that
  timer (before Windows 10 1803) it waits in whole milliseconds only while
  more than two 16 ms ticks are left and spins on the done word, looking at
  the event, for the rest, so it ends at the deadline (ADR-0102).
  Only the wake and each timed wait are inside `rt::ScopedSanctionedSyscall`,
  so the rtsan preset checks the rest of the path; the background wait
  (ADR-0013: `alive()`, kill) keeps one scope over its loop. The rtsan-only
  test `rtsan-bridge-selfcheck` makes a bridged call under a live deadline,
  which must pass, then allocates, which must be caught.
- The bridge is synchronous everywhere. Offline and on rendered-ahead nodes it
  keeps the background deadlines below. On device-callback nodes (armed and
  monitored tracks, input FX, take FX stages, and every node with
  anticipation off) the callback and its live helpers process bridged stages
  and make their lifecycle calls under the cycle's bridge deadline (ADR-0081):
  `Engine::cycle` sets it to the cycle's start plus its period or
  `EngineOptions::liveBridgeDeadlineMs` (default 2 ms), whichever is larger.
  No latency is added and the output is the in-process output. A cycle with a
  late bridged call counts in `Engine::bridgeLateCycles()`,
  `engine.status.bridge_late`, and in `engine.status.xruns`. A pipelined mode
  remains future work.
- Bridge timing. Every bridged call records where its time went
  (`host::bridgeTiming()`, live and background apart): the host's wait, the
  bridge's wake (post to seen), its work and the return (finished to seen by
  the host), wakes over 200 µs and 1 ms, whether the answer came while the
  host still spun, the CPUs the bridge thread ran on, its migrations, and
  calls whose bridge thread did not hold the class asked for (Linux). A wait
  for an earlier late block before the next call (`settles`) counts its wait
  too. `Engine::bridgeOverruns()` sorts each callback cycle with live bridged
  calls that ran past its period. When the cycle's bridged waits cover the
  overrun it is `late_wait` (a call hit the deadline) or `slow_bridge` (the
  bridge answered in time but slowly); otherwise it is `not_waiting`: the
  time went elsewhere, such as another stage's work or the callback thread
  waiting for a CPU, including a call that was late because it was posted
  after the deadline had passed. The longest such cycle is kept.
  `engine.status.bridge_timing` reports both; the call counters are
  process-wide, the cycle counters per engine.
- Crash handling: a control thread watches the process (pidfd); on crash the
  instance becomes a **failed node** (silence; `fx.failed` event with the
  signal and last known state kept in the model; when the bridge died of a signal or an exception
  and a local crash-report supervisor was there, `incident` names the report that death may have
  caused (ADR-0083), also in `engine.status.failed_errors[]` once the bridge's end was observed;
  the report may not exist, and `crash.read` says so), the rest of the session
  keeps playing, and `fx.restart` re-creates it from the last known state.
  In an offline render, a node that fails this way fails the render instead
  (§3, ADR-0090).
- Acceptance: a deliberately crashing test plugin (a CLAP that segfaults on
  a given parameter value) in `dedicated` mode does not stop the engine; a
  real-time capture around the crash shows only that node going silent.

## 6. Control surface

The headless `play` CLI parses each numeric option completely and rejects
malformed, overflowing or non-finite numbers with `cli.number`, the option's
field path and a corrective hint before loading a project or opening a device.
`--seconds` accepts finite nonnegative durations; integer options require whole
decimal integers. Command schemas still validate the device/transport ranges.

The engine registers the session-model §7 command catalogue on the command
bus and exposes it through the control protocol (Unix socket), Lua and MCP (proxy or in-process). Engine-specific commands:
`engine.start {backend, device, rate, block}`, `engine.stop`,
`engine.status` (q: backend, rate, block, load, xruns (device xruns plus
`bridge_late`, the callback cycles a bridged plug-in made late, ADR-0081),
`bridge_timing` (the live bridged calls' waits and why cycles overran, §5),
anticipation misses, meter drops), `engine.settings.set {render_ahead_ms,
worker_threads, …}`.
`audition.play {path}` (a WAV, FLAC or MP3, absolute or a name in the
project's media folder; returns `path`, `seconds`, `sample_rate`,
`channels`, `truncated`), `audition.stop` and `audition.get` (q: `playing`,
`decoding`, `path`, `seconds`, `position`) play a file on outputs 1/2
outside the project ("File audition" above, ADR-0094); errors
`engine.stopped` and `audition.file`, a failed decode the event
`audition.error {path, code, message}`.
`OfflineResult::graph` supplies compiled graph text: input edges and residual
padding, `lat_in` (the forward arrival diagnostic), `lat_chain` (compensated
chain latency), and `timeline_shift` (processing position minus node-input
timeline position). There is no registered `engine.graph` command.
Events: `transport.changed`, `engine.xrun`, `engine.stats` (1 Hz),
`meters` (rate-limited, subscribe per track), `fx.failed`,
`record.progress`.

The GUI, headless socket and stdio MCP servers drive **main-thread service** on
notifications and the nearest deadline (ADR-0030), using `EngineApp::tick()`
as the explicit service call. `serviceFd()` exposes the app's coalesced relay;
`serviceFds()` snapshots local plug-in descriptor subscriptions (including
persistent probes and retired instances). `serviceFdReady()` checks that the
owner and registration still exist before invoking a plug-in. Consumers cancel
old watches before owners/fds can be destroyed or reused. Remote instances use
the same `setMainThreadWake` subscription contract; the bridge owns forwarding.

`tick()` drains/rearms the relay before consuming work, services instances,
consumes outputs and control MIDI, captures dirty state, reports failures,
handles backend changes and autosave, then flushes dispatcher work/job events
and delivers Lua events in the same pass. Retained work blocked by a model
transaction resumes on commit/rollback. Inactive instances and persistent
probes keep their main callbacks/timers/FDs serviced; probe outputs do not edit
an unrelated FX. Finished worker threads notify and are joined on main.

Producers notify after publishing control MIDI (including drops), outputs and
output/voice drops, executor plan adoption, epoch/state transitions, failures,
backend status changes, job events and posted work. Unchanged audio blocks do
not wake main. JACK period changes update status; rate changes/shutdown trigger
restart. The installed backend relay outlives client close and is detached
afterwards.

`nextServiceDelayMs()` returns the minimum remaining service delay, or none;
`nextServiceDeadline()` maps that delay to an absolute steady-clock deadline.
Service time is monotonic (`serviceClockMs()`); command/history timestamps stay
wall-clock. `setFixedClock()` drives both for deterministic tests. Only changed,
named projects arm autosave, at 60 s since the last save/attempt; an overdue
edit is serviced immediately, and write failure waits another 60 s. Plain
learned controls commit after 300 ms at rest, except while a transaction is open.
Local plug-in timers participate even without an editor. Device restart uses
an immediate attempt, then 250/500/1000/2000/4000 ms retries, with the 30 s
give-up deadline taking precedence over a later retry. Give-up retains the
last backend error even when it expires before another attempt.
GUI meter consumers observe `meterActivitySequence()` before draining
`meters()`, then call `armMeterActivity(observed, minimumPeak)` only when visible
meters reach rest. Arm rechecks intervening publication. The default GUI floor
is 0.001 linear (-60 dB); sub-floor readings do not wake main. Moving meters
use GUI frame deadlines and remain disarmed; there is no per-block meter wake.

`engine.restarting {reason}` announces restart; exhaustion emits
`engine.error {reason, error}`. A restart whose device has not taken the
restored position within 1 s emits `engine.error {reason, restarted: true,
error}`: the engine runs, and the position stays queued.
The backend kind, block/options, transport position and loop are restored;
playback stays stopped. There is no global main-thread safety poll.

The GUI shares one FD watcher for the app relay, local plug-in descriptors
and ModelLoop's wake pipe. Its nearest panel/service deadline bounds SDL's
wait; there is no service/control poll. Editor service does not duplicate
app timers, and remote editor work relies on the bridge wake contract.

The tick drains the devices' **control messages** for MIDI learn
(ADR-0027): each callback copies its notes, CCs, program changes and pitch
bends into a 1024-entry SPSC ring (`Engine::takeControlMidi`; overflow
counted, `controlMidiDrops`), and `EngineApp::pollControl` drains it, hands
the messages to `onControl` (the window app's action bindings and learn
dialog) and moves the plug-in parameters learned for them (fx `learn`): a
semantic parameter through `fx.param.set` with one gesture per movement, a
plain one on the running instance at once and into the project when the
control has rested for 300 ms (as an edit in the plug-in's own editor).
Then the tracks', sends' and VCAs' own bindings (track/route/vca `learn`,
ADR-0058) move through `moveControl`: a `track.set`, `route.set` or
`vca.set` with one gesture per movement, so group links, the strip rules
and touch/latch/write recording apply as for the fader. A level follows the fader's travel (`supple::fader`),
a button flips per press (a note, a program change, a CC in toggle mode) or
follows an absolute CC (on above 63); a frozen strip's bindings move only its
volume, pan, mute and solo.
Nothing moves while `setLearning(true)` (a learn dialog listens); bindings
with `when_*` conditions ask `learnGate` and wait without one. The callback
publishes a batch and wakes main at once; the GUI needs no binding timer.
`lastTouched()` is the
parameter edited last from the window or a plug-in's editor (not by agents or
controls); `moveParam` moves any parameter the way a binding does, for the
"last touched parameter" action.

## 7. Benchmarks (acceptance, `docs/benchmarks/`)

- **B1 capacity:** generated session: N tracks, each a stock synth
  (Marram's pad in `host/engine/bench/session.json`; Orrery's reference
  patch until 2026-10-21) + stock EQ + stock
  compressor playing a 4-bar notation loop, one armed audio track with the
  same chain monitoring input; at 48 kHz / 64 frames on this machine: find
  the largest N with zero xruns over 5 minutes, with and without
  anticipative processing. What matters is the capacity itself, above all
  with anticipation, and how it compares with REAPER's (B2); there is no
  required ratio between the two modes (2026-10-21: the earlier target,
  anticipative on ≥ 3× off, is dropped).
- **B2 REAPER comparison:** the same structure in REAPER 7 (the stock synth
  loaded as CLAP in REAPER, ReaEQ/ReaComp substituted where needed and
  noted), same device settings, anticipative FX at its default; report N for
  both.
- **B3 latency/PDC:** impulse through chains with known latencies across a
  folder/send topology; outputs must align to the sample.
- **B4 determinism:** repeated offline renders (1, 2, 8 worker threads)
  byte-identical; real-time capture vs offline null.
- **B5 edit responsiveness:** 1000 random edits via the bus while playing:
  main-thread time per edit (p95 < 2 ms), no xruns, plan swap latency.
- **B6 isolation:** crash test (§5) + CPU overhead of synchronous background bridge vs
  in-process for 16 instances. `bridge-bench` (host/clap-host/tools) times
  bridged against in-process calls on the gain fixture per block size,
  stage count and isolation, paced like a callback; results are in
  `docs/benchmarks/b6.md`.

Results are recorded with kernel, PipeWire version, quantum actually
granted, and CPU governor (read-only facts).

B1/B2 tooling: [tools/bench/README.md](../../tools/bench/README.md).
The command-built shared session, paced search and REAPER offline generator
are dry-verifiable. Real measurements require `SUPPLE_AUDIO_DEVICE_TEST=1`
exactly. Historical device results are in `docs/benchmarks/b1.md` and
`b2.md`; their recorded revision, settings and duration bound the claims. The
search retains every trial, independently confirms its candidate, and labels
a passing search cap as a lower bound. REAPER counts observed audio/media
underrun timestamp changes (multiple events between polls can be undercounted).

### 7.1 Benchmark-only measurements

`EngineOptions::benchProbe` and `JackOptions::benchProbe` default to null.
Only `engine-bench` enables these pointers; there is no GUI preference.
`BenchProbe` owns preallocated, bounded single-producer sample arrays, one for
the backend and one for the renderer. Allocate it before start, retain it until
both stop, and inspect it only after producers join. One measurement interval
per probe; an atomic gate excludes preroll. Newest samples are dropped and
counted on overflow; a benchmark with dropped samples is invalid. No sorting,
allocation, formatting or logging occurs on a producer. Disabled probes read
no clock and allocate no engine storage.

The JACK timer surrounds the whole process callback, including all engine
chunks and backend bookkeeping in a granted period. Paced timing surrounds
one synthetic callback. Benchmark input is silence and physical outputs are
silenced after measuring the pre-output peak. Render-job timing surrounds
playing `processRendererBlock`, including pool join, excluding ring publication
and idle/pacing waits. Nearest-rank p50/p99/p99.9/max and mean are in microseconds;
load divides total time by the captured frame duration. Timing start/end reads
add measurement overhead. A sample begun before the gate closes may finish
after it; aggregate status snapshots can differ by one boundary cycle.

Ring low-water is a callback observation before consumption, after priming in
the current epoch: published renderer stream frames minus consumed stream
frames, clamped to zero. Publication of the stream counter can lag ring
publication; this is a conservative observed ahead distance, not an atomic
snapshot of every frontier ring or an assertion about minimum wake latency.
It is null when there is no sampled anticipative boundary (including an
all-live graph); observations and anticipation misses are reported separately.

`tools/bench` accepts 64/128-frame requests. Run JSON is
`supple.capacity-run/2.5` (`tools/bench/README.md` lists what 2.2 to 2.5
add: scheduling, `bridge_timing`, on Linux per-thread `threads`, and page
faults per thread and as `engine.status` counts them, and the idle-latency
hold and huge-page setting in effect): `xruns` is a JACK server counter delta or null for
paced runs; `live_helpers` reports enabled live capacity (0: serial);
`synthetic_over_budget` and `synthetic_late_wakes` are paced-only
counters (null for JACK). Callback `over_budget` is a duration test, never a
hardware xrun count. Requested block, granted quantum/rate and
`requested_settings_granted` are separate. Mismatched grants remain useful
fixed-run data but cannot pass a requested-setting capacity search. A model
tick that retires the engine after device loss aborts the run with
`bench.device_changed`, cancels automatic restart and never samples the retired
engine. No forced quantum or rate is applied. Optional third-party effect
fixtures take an
installed module path and descriptor ID; binaries are never vendored by the
harness. See the tooling README for schemas, limits and invocation details.

## Implementation notes (2026-09-26)

- **Code layout:** `host/engine/src/plan.{h,cpp}` is the shared compiled plan
  (compile on the main thread, prepare = activation/latency/buffers/events,
  `processBlock` and `locate` on the audio thread); `offline.cpp` drives it
  for `renderOffline`, `realtime.cpp` for the `Engine`. Real-time output is
  bit-identical to the offline render for block-size-invariant plugins
  (tested at 128-frame device blocks against 512-frame offline blocks, with
  and without plugin latency).
- **Plan hand-off:** `RtHandoff<Plan>`. `acquire()` returns the previous
  plan to the main thread immediately, so the audio thread keeps its own
  preallocated list of the instances it is processing; on adoption it stops
  instances the new plan dropped and starts new ones, then stores the plan's
  generation; the main thread prunes the instance pool only once that
  generation reaches the newest published one (`service()`), so no instance
  is destroyed before the audio thread has stopped it. It prunes at the
  generation it freed old plans at, read once: an adoption after that read
  waits for the next `service()`, so no plan the main thread keeps (and
  services) names a destroyed instance. An executor takes a
  free instance by a compare-exchange of its owner token from none
  (`Instance::claimOwner`), so the renderer and the callback, both finding
  one free, never both start it. A plugin whose `start_processing()` fails
  is left out of processing (its stage passes audio through,
  delay-compensated, and `processErrors` counts it) and is reset as it
  refuses, so a retry that succeeds starts it clean; the next plan retries
  it. A bridged plug-in whose start is not answered by the live deadline
  (`Instance::startUnanswered()`, ADR-0081) has not refused: it is reset and
  let go the same way, uncounted, and the next cycle looks again; one that
  never answers fails as a hung bridge at the lifecycle deadline (ADR-0101). Per-stage event storage is reserved for the most events any block-long
  window holds, so a seek off the block grid never allocates. Plans share
  ownership of the session's TimeMap (a session version may be freed while
  its plan plays).
- **Transport:** play/pause/seek/shutdown are coalescing atomics (the latest
  request wins, none is dropped however many arrive between callbacks).
  Shutdown: `Backend::stop()` promises one final zero-frame callback on the
  audio thread (a manual driver makes it on its caller, which is its audio
  thread), and the engine stops every plugin there — no timeout, no stop from
  the main thread however long the device's period. In the in-callback
  executor, a device cycle that runs after the request and before that
  callback stops the callback's plug-ins and then plays silence: it adopts
  no plan and starts no plug-in, not even a refused one's retry. In the
  anticipative executor the renderer stops its share as it ends, and the
  callback's live region may still adopt a plan and start its plug-ins in
  such a cycle: the final callback stops every live instance whatever it
  holds. The final callback also stops whatever the in-callback executor
  still holds, and `Engine::stop()` stops any instance still processing
  once every audio thread has joined. So a stop never leaves an instance
  processing without an owner, which an engine restarted on the same pool
  could never claim. Pause and
  seek `reset()` the plugins (no hanging notes); seek re-locates event and
  automation cursors and clears delay lines, a rack's plug-in stages' dry
  delays and oversampling filters as a chain's (`locateRackStages`). A
  seek, and a play from a position, also restart every rack module
  (`rack::Instance::restart()`)
  and its feedback cables' history (`rack::Schedule::restart()`), once the
  executor owns the rack (ADR-0062 §5, 2026-11-14 note), where the
  plug-ins are reset: the rack then plays as an offline render from there.
  A loop wrap restarts neither. Timeline output lags the
  processing position by the plan's output latency; monitored input incurs
  its own downstream path latency (ADR-0051).
- **Mixing decisions:** instruments add their output to the track's input;
  effects mix wet/dry against the input delayed by their latency; bypass is
  compensated (a delay of the stage's latency); the pan law applies to tracks
  (ADR-0066: each channel's and buss's own or the project's), the master pans
  as a balance; mute silences the post-fader tap only; a
  solo-silenced track is still processed. *(Superseded by ADR-0019 for
  strips: Console 9 stages, mute as a fader ramp before the encode.)*
- **JACK backend** (`jack.h`, `jack_backend.cpp`): libjack is dlopen'ed
  (default `libjack.so.0`) through our own declarations of the API subset we
  use, loaded once per path and never unloaded. The engine block is the
  largest callback: a device period larger than the block is split into
  block-sized calls (an odd period leaves a shorter last call), so period
  changes need no restart. The block is requested from PipeWire as the
  client's latency (`PIPEWIRE_LATENCY=<block>/<rate>`, set only for the
  `jack_client_open` call and only when the user has not set it) — a request
  the graph honours unless another client needs a longer period;
  `PIPEWIRE_QUANTUM` is not used because pipewire-jack maps it to
  `node.force-quantum` + `node.force-rate` (the whole graph).
  An explicit rate (`JackOptions::requestRate`, engine.start's `rate`) is
  suggested as `PIPEWIRE_RATE=1/<rate>` (`node.rate`: applied only while the
  devices are idle and the rate is allowed). `forceGraph` (engine.start's
  `force_graph`) sets `PIPEWIRE_PROPS={ node.force-quantum=<block>
  node.force-rate=<rate> }` (the rate only when given), forcing both on the
  graph while the client is active (ADR-0050). Every variable is set only
  for the `jack_client_open` call, and one the user set wins.
  `forceBufferSize` (`jack_set_buffer_size`) is the opt-in for benchmarks.
  The granted rate is the engine's rate, read once the client has run its
  first cycle (at most 1 s after activation): before that pipewire-jack
  reports its default rate or the latency request's denominator, and its
  rate callback flips while the client joins (a graph a browser started at
  44.1 kHz reads as 48 kHz). Only a later reported rate that differs from
  it is a rate change. A project at another rate plays at the device's
  (positions are in seconds, media is resampled to the plan's rate, plugins
  are activated at it; renders stay at the project's rate). Stop:
  the next process cycle makes the final zero-frame callback; if the server
  is gone or no cycle comes within 2 s, the client is deactivated and closed
  and the engine stops its plugins on the main thread. Xruns (server
  callback), rate changes and server shutdown are atomics read by
  `engine.status` (`restart_needed`); the model thread's service tick (§6)
  restarts the engine after a device change. Tests use a stand-in libjack
  (`host/engine/tests/fake_jack.cpp`) that drives the
  process callback from its own thread and injects period/rate changes,
  xruns, stalls and shutdown; the real server is used only when
  `SUPPLE_AUDIO_DEVICE_TEST` equals exactly `1` (unset, empty, `0`, and
  all other values skip it):
  `SUPPLE_AUDIO_DEVICE_TEST=1 engine.tests -tc='real JACK*'` (2 s of silence
  at a 64-frame block). First run on this machine: 48 kHz, period 128 for a
  64-frame request, 0 xruns, final callback on the JACK thread.
- **Control:** `engine.start {backend: jack|dummy, block, rate, force_graph}`, `engine.stop`,
  `engine.status`, `transport.play|pause|stop|seek|get`. Transport actions
  wait (≤ 1 s) for two audio cycles so their answer is the new state; if
  the device has not run them by then (a stalled device, a starved
  renderer), the action is refused with `engine.transport`, whose hint
  points at `engine.status`, rather than answered with the old state. The
  request stays queued and takes effect when the device runs again, so it
  is not to be repeated. A
  recording started from stop under a scheduled playrate waits the same
  way for the renderer to take its start, and is refused alike; a restart
  whose device has not taken the restored position reports `engine.error`
  with `restarted: true` (below). The 1 s is `transportWaitSeconds()`: tests raise
  it to 60 s, so a starved runner never becomes a refusal (in process with
  `setTransportWaitSeconds`, for a binary a test starts with
  `SUPPLE_TEST_TRANSPORT_PATIENT=1`, which every engine and GUI ctest entry
  sets), and a test of the refusal sets a short bound of its own.
  `transport.stop` returns to where play started. `<slug>-engine play FILE`
  plays a project through the device from the CLI.
- **Anticipative processing** (ADR-0012, `EngineOptions`, default on): a
  renderer thread owns the plan, the plugin lifecycle and the render
  position, renders the whole graph (nothing is live yet) in 1024-frame
  blocks into an SPSC ring of epoch-tagged chunks kept 200 ms ahead, and the
  callback only copies chunk frames. Seek and pause bump the epoch on the
  main thread; the callback drops older chunks at its next cycle (pause is
  silent at once, a seek never plays stale audio); the renderer relocates —
  after a pause to the position the callback reports once it has seen the
  new epoch. New plans are adopted at the render position (edits are heard
  after the audio already rendered, no gap). Renderer parking follows
  ADR-0028: stopped block deadlines and event-driven buffer-space wakes
  from the callback, with a safety bound of half the playback buffer span
  (1 s while stopped, held or all live). Misses (no chunk while playing, after
  the pre-roll) are counted (`engine.status.anticipation_misses`). The
  pre-roll of a start or a seek lasts until the renderer has rendered a whole
  render block of the epoch and the callback's whole cycle (that at most the
  render-ahead span), in the output ring or the frontier rings: never a first
  chunk cut short, such as a launch from stop whose compensated tracks switch
  a few frames in, or a loop wrap. It also ends when a ring the
  renderer writes is full, so it can render no more until the callback
  plays: a loop shorter than the rings' chunks fills them first (see "Rings
  are recycled"). Until then the
  callback plays silence and time holds still. The pre-roll ends with the
  first frame played: the rest of that cycle, past a loop wrap too, plays on.
  A cycle that has played audio and then runs short counts a miss, the cycle
  that ends the pre-roll included. *(Amended 2026-11-21,
  ADR-0012 note: priming took any chunk, so a launch from stop played its
  first frames and then a gap, counted or not.)* `stop()`
  joins the renderer first; it stops every plugin as it ends. With
  `anticipative = false` the old in-callback renderer runs (the reference,
  and the base for live nodes).
- **Worker pool** (`workers.{h,cpp}`): helper threads (every allowed CPU but three,
  plus the submitting thread) run one render block as a DAG over the plan's
  nodes: per-node atomic counts of unfinished sources, a bounded Vyukov MPMC
  ready queue, the submitter helps; graph-bounded participation and idle
  parking follow ADR-0023. The submitter joins every enrolled helper.
  `processNode` is the unit (a node sums its inputs in the fixed order and
  processes its own edges, items and chain), so results are bit-identical
  for any thread count — the offline renderer uses the pool too (B4 test:
  1, 2, 8 threads byte-identical) and defaults to all cores.
- **Plugin isolation** (ADR-0013): `dedicated` fx run in `<slug>-bridge`
  processes (control socketpair + memfd data plane + futex, synchronous with
  a 2 s deadline (lifecycle commands 7 s)); a crash or hang fails only that instance (silent node,
  `fx.failed`, `engine.status.failed_fx`); `fx.restart` re-creates it from
  the model. Acceptance test in `engine.tests`.
- **Plugin-parameter automation:** an envelope on `fx_* <param>` resolves the
  parameter from the plugin's semantic schema (`params::fromDescription`
  rebuilds its ParamSpec, so the host converts exactly as the plugin does)
  or, for any plugin, by parameter id or name (plain CLAP values), through the
  one resolver session-model's "Plug-in parameter references" describes.
  Semantic points
  are physical and interpolated in the normalized (curve) domain (ADR-0010).
  The stage gets param events at absolute timeline sample positions that are
  multiples of 32 in the lane's latency-shifted coordinates, and at each exact
  point (in time order; unchanged values are not resent). A block-start value
  is sent only to initialize a lane after prepare, locate, seek or loop wrap,
  merged into its sorted event list by an allocation-free insertion sort,
  within capacity reserved at prepare. Lanes shift by the node's solved
  timeline shift plus the stages before them (minus take latency for take
  FX); seeks reset them and resend. Real-time output equals the
  offline render. Commands: `env.add`, `env.points.set`, `env.set`,
  `env.remove`, `env.list` (values in the parameter's unit; text like
  "800 Hz" for plugins). Write/touch/latch recording uses the envelope's own
  timebase, including points received from plugin output events (§1.4).
- **Recording and input monitoring** (ADR-0014): see the ADR's
  implementation notes (recording; the live region with two executors;
  crash recovery).
- **Loop and punch** (ADR-0015): see the ADR's implementation notes.
- **MIDI input** (ADR-0016, ADR-0065): `record.midi_input` names a
  channel's MIDI route; old MIDI-only `record.input` remains readable.
  Hardware audio, MIDI and rack Out inputs replace each other (ADR-0074).
  In input record mode a selected audio or rack input records audio. MIDI
  overdub/replace records delivered MIDI sends instead, independently of the
  selected input.
  `record_midi: null` clears MIDI alone, `record_input: null` hardware audio
  alone and `record_rack: null` a rack Out alone. Clearing all three leaves
  no input. A rack-fed channel records its audio as a WAV take. Feed Notes
  through a MIDI send from a separate monitored MIDI channel; arm that
  channel to record notes, or both channels to capture notes and rack audio
  in one pass. A rack's Notes
  module follows its source channel's armed monitoring and channel map, in
  both executors. Explicit Notes devices still listen always. A live-input
  change ends only the notes and controllers live events opened (a
  generation in the Notes feed, seen on the audio thread). On a strip, a
  plan that no longer hears the same MIDI input (disarmed, monitoring off,
  a monitor-mode, device or channel change) ends the device's voices only;
  timeline and rack-delivered notes play on, in both executors.
- **Class changes while playing** (ADR-0014): a node the anticipative
  engine moves into the live region (arming or monitoring mid-song)
  continues at the handoff where the renderer left it. Playing, the
  renderer hands its instance over without `reset()` and carries the
  node's voices, held notes, delay lines, Console 9 and fader state and
  take and input chains into the new plan (`carryMix` from its class into
  the live one; `Plan::handContinuable`), from a plan of the same kind:
  two unity plans, or two master-playrate plans adopted at their prepared
  handoff without a seek (`rateAdoption` is ready only there, where the
  plan's hand device position is the renderer's own). The first plan under
  a rate, after unity, is not continuable: the device coordinate changes
  there. (Once a rate plan is prepared, every later plan is one until the
  engine stops, back at 1× too, so this is the only such step.) The
  callback continues such a node only when it adopts the plan exactly at
  its handoff, playing, in the same epoch; then held timeline notes, racks
  (their Notes keys included) and effect tails sound on and the output
  stays the offline render's, at unity and under a master playrate or its
  envelope. Otherwise (stopped, a seek or new epoch, a late adoption after
  a frontier miss, or the switch from unity to a rate) the nodes entering
  the live region start afresh: voices and held notes cleared, plug-ins
  reset, racks restarted, so nothing hangs, but a held note cuts. Bridged
  stages take part as in-process ones do (§2.1, §5). A node leaving the live
  region stays live while the transport plays (`keepLive`).
- **Smooth swaps:** a plan swap carries each strip's fader ramps and Console 9 state
  and each insert's applied wet level, a rack's plug-in stages' too, into the new plan
  (`carryMix`, by track and by instance, per executor); a changed volume,
  pan, mute, solo, FX wet or bypass then ramps over 10 ms instead of
  stepping (a bypassed plugin keeps running until faded out). Automated
  values follow their envelope; mute/solo retain a separate 10 ms ramp before
  encoding. A Console 9 mode change crossfades each implicit stage over 10 ms,
  keeping user inserts on their single processing path. Dither states persist
  through edits and restart on transport locates; source-owned send encoders
  run before compensation delays and frontier rings. Channel/buss post-fader
  stems get a neutral decoder. Structural changes (a plugin inserted or
  removed, a route added) still switch at the swap.
- **Media resampling:** `MediaCache::get(path, rate, variant, contentHash)`
  decodes and resamples once per (media identity, rate, variant) with the
  audio-file polyphase resampler (Kaiser
  windowed sinc; deterministic, so offline ≡ real time); plans ask for their
  own rate. The real-time engine compiles at the device's rate, so a project
  at another rate plays (previously refused). The cache is in memory and the
  resampling is synchronous at plan compile. A supplied `MediaSource.hash`
  identifies both decoded audio and its variants, independent of pathname.
  Without a hash, identity is device, inode, size and nanosecond mtime,
  inspected on the model/decode thread at preparation (and checked again
  after decode). A changed identity gets new immutable buffers; old plans
  keep their leased buffers. Decoded originals are weakly indexed: they live
  only while a plan or passthrough variant leases them; expired entries are
  pruned on variant misses. Additional variants may decode the original again
  after its last lease ends. Resampling and stretching read immutable input
  directly, without a preliminary full-buffer copy. Disk backing and background
  decode remain planned.
- **Take playrate and pitch:** a take's playback variant is built once per
  (media identity, plan rate, variant) in the same cache: varispeed resamples to
  `rate / speed` (`pitch_mode: "resample"` or `preserve_pitch: false`), the
  default `"signalsmith"` mode stretches with Signalsmith Stretch (MIT,
  seeded, so deterministic) and transposes by `pitch`. The stretch runs at
  plan compile on the main thread (~0.3 s per source second in a debug
  build); it moves to the background decode with the disk cache.
  `stretch_markers` use prepared item-local warped audio (ADR-0053; see below).
- **Live pool** (`live_workers.{h,cpp}`): callback fork-join as specified in §2.2.
- **Not yet:** crossfades for
  structural changes (inserting or removing plugins). B1/B2 measurements:
  `docs/benchmarks/b1.md`, `b2.md` (tooling: `tools/bench/README.md`).

### Recording diagnostics (2026-10-04)

**Durability (ADR-0082):** the recording writer flushes each drain and syncs
audio data and header at the MIDI journal's shared five-second deadline and
at stop, through the same core-base helper (fsync / macOS full-sync-or-fsync /
FlushFileBuffers). New audio and journal directory entries are checkpointed
off RT too. The cadence assumes healthy storage making progress; a stalled or
failed barrier is not a successful checkpoint. A sync failure latches
`file.sync` / `file.sync_directory` as a target failure and reaches `record.error` through the model
wake, like an append failure. Healthy targets and the engine continue.

Copied/moved media is synced, then published without replace, and its directory
is checkpointed before the referencing project is published. The project is
confirmed durable before originals are removed. A failure after project
publication returns `project.sync` and retains both originals and copied media;
its hint explains that the project may already exist and storage needs checking.
On Windows these operations use only unprivileged FlushFileBuffers and
MoveFileExW with MOVEFILE_WRITE_THROUGH for media publication. This matches the
project-save level of file durability; it does **not** establish POSIX-style
parent/ancestor namespace durability after a power cut. No elevation is used.

Audio writer failures (ADR-0082) latch the first error per target and wake the
model thread. `Engine::takeRecordingErrors()` consumes these off-RT notices
(`track`, `file`, structured `error`); EngineApp emits `record.error` immediately,
with `track`, `file`, `error` text and `code`, `message`, `path`, `hint`.
Stop reports a not-yet-delivered failure, without duplicating an earlier event.
`RecordedTake::failure` retains the structured first error for that stop/close
fallback, beside the existing `error` text.
The failed audio target stops writing and retains every complete frame physically
written, including a short-write prefix; its final header and take use that
count. Healthy targets and playback continue. Recording file creation, writes,
prefix accounting, header updates and closure run on the recording writer;
the model joins it and commits metadata only. The GUI shows the error in its
status line while recording continues for healthy targets.

A committed recording may carry a `warnings` array with structured
`code`, `message`, `path`, `hint`, `track` and `file` fields.
`recording.loop_runs_full` is forwarded to `transport.stop` / pause callers
and the `record.warning` event while retained passes still commit. Every
warning a commit itself adds (where a take went: `record.stack_skipped`,
`record.stack_group` and the other `record.*` warnings of take stacking,
ADR-0103, and those of the `take.comp` it makes) also goes out as a
`record.warning` event, in the same shape, once the commit has succeeded,
whatever stopped the recording (a command from any client, or the engine
halting, whose commit has no caller to answer); each warning once per
commit, a pass's own warnings not again.
`project.recoverable` and `project.recover` expose skipped invalid-sidecar
diagnostics in the same optional `warnings` shape (`record.sidecar`, and
`record.journal` for a MIDI journal whose header does not read). Their
`file`, like the media path a committed take is stored under, is relative
to the media folder with `/` between its parts on every platform
(`rec/Vocal-1.wav`).
`project.recoverable` lists a MIDI journal with at least one event that no
committed take of the project holds (`midi_events`: how many; `channels`
0); `project.recover` places it as stop would have, with its Note Out route
ids dropped where the project no longer has the route; each such voice
keeps its own identity, so overlapping notes of one pitch pair as stop paired
them (ADR-0080). Recovery
skips corrupt selections and continues with valid recordings; an unknown path
still fails, and a directory with no recordings or diagnostics reports
`recover.nothing`.

### Plug-in notifications and output events (review round 3)

The shared `model::SourceBeatMapping` defines MIDI take offset and playrate
conversion for playback and thumbnails. Looped offsets are cyclic modulo the
source length; playback includes the preceding cycle's crossing notes, chases
notes held at item start, and clips releases to the item end. Drawing retains
its bounded visible-pass enumeration; that drawing limit does not cap playback.

`InstancePool::service(fx)` services owned instances on main and drains
`restartError`. A refused reactivation is retained in `failedErrors()` as
`{fx, Error{code,message,path,hint}}`, also appearing in the existing string
`failed()` API for compatibility. Its stage passes through with the last active
latency; preparation does not retry it. `fx.restart` replaces it and clears the
failure. The app still needs to publish the structured error in `fx.failed`
and engine status (its current consumer publishes only the reason string).

Local CLAP `request_process` is an atomic wake request. The audio owner consumes
it at the next processed block. With `auto_bypass_on_silence`, a stage may idle
only after `CLAP_PROCESS_SLEEP`, with exactly silent main/key inputs and no
input events or pending flush. Notes, automation, nonzero input, queued main
parameter changes and `request_process` resume it. A stopped transport keeps
running every plugin without advancing its position (ADR-0014): the callback's
plan each cycle, the renderer's share at render-block pace (a request can
run early, at least 1 ms after the previous stopped block finishes); play
re-locates the non-live nodes and resets their plugins.
A plain plugin's state reaches the model when it calls `clap.state
mark_dirty` (a coalesced "Plug-in state" step per plugin) and, for every
running plugin, before each save: a sampler's loaded files are saved even
when it never says so (LSP's multi-samplers do not). A plugin that cannot
report its state then keeps the state saved before, and `project.save` /
`project.template.save` name it in `warnings` (code `fx.state`, path
`/fx/<id>`); the GUI shows them after a save. An inactive
instance is activated on main using its last activation configuration; a never
activated instance is configured by plan preparation. Explicit bypass is kept; queued note/parameter events are still processed with
compensated dry output so note releases are not lost while bypassed.
Bridged wake and clear forwarding need the control-protocol extension below.

Rescan requests rebuild an immutable plan from the current parameter list and
schema; envelopes are resolved again, including their value-domain conversion.
A rescan of values or their text only (`CLAP_PARAM_RESCAN_VALUES`, `_TEXT`)
rebuilds it only when the FX has an active envelope, whose values the new
plan sends again, or had automation cleared, which the rescan re-enables: a
plan holds no parameter values (ADR-0092). A rescan without flags counts as
one of the list.
Missing/read-only parameters stop receiving events and produce a warning naming
the FX and parameter. Clear `ALL`/`AUTOMATIONS` suppresses only the matching
parameter's automation until a subsequent rescan or explicit engine load. Clear
`MODULATIONS` has no automation effect (the engine has no modulation lanes).
Envelopes remain in the model. A rescan after a clear in the same service batch
re-enables it; a clear after a rescan remains effective. Changes are heard at
normal plan-adoption boundaries, including the anticipative edit horizon.

`Engine::takePluginOutputs()` is main-thread only. Each record contains FX id,
plan generation, the patch sequence read before its block was processed
(ADR-0092; idle flush records: when drained), transport epoch, timeline
sample, stream sample, the entire
`host::OutputEvent`, and idle/anticipative markers. Note ends and plug-in
note/MIDI output (ADR-0068) are not published there. Timeline coordinates subtract
the stage's input timeline shift (§1.3); idle flush records use the service-time
play position.
Parameter values and gesture boundaries are copied after worker tasks finish,
into separate preallocated 4096-entry SPSC rings for renderer and callback.
Publishing a batch (or a drop) notifies main after queue publication. Outputs
rendered ahead of playback are retained until their epoch's played stream
reaches their stream sample. Main arms the earliest pending target with
`Engine::armOutputWake(epoch, stream)` (`INT64_MAX` disarms); callback consumption
notifies only once per arm. Main rechecks after arming, so a crossing cannot be
lost. Seek/stop/plan changes discard stale pending records and rearm. A model
transaction defers model edits and disarms the target until it closes.
Per-executor FIFO is preserved; there is no global ordering between these queues
and idle flushes. Output arenas have 256 entries per stage and idle staging has
4096 entries. Overflow drops newest records and is exposed as a cumulative
`dropped` counter, including host flush/arena drops. This bounded transport is
not the future reliable coalescing channel for automation recording. The consumer
must detect loss and resynchronize values and incomplete gestures.

NoteEnd is consumed on the audio side. Held voices match all specified
`(port, channel, key, noteId)` fields; every `-1` is a wildcard, and id `-1`
uses the tuple. Running note IDs differ from compiled IDs so an old loop's end
cannot terminate a retriggered voice. Each note stage reserves 4096 held voices; excess onsets are dropped before DSP
so every onset admitted by the engine has a release mapping;
`voiceDropped` counts overflow cumulatively (offline renders report warnings).
The engine tracks held input notes, not the plug-in's internal release tails.
The host input arena has its own 8192-event cap; recovery for a dropped release
there remains a reported policy/integration issue.
Plan adoption carries running IDs and held addresses, rebinds compiled IDs by
musical source origin/address/duplicate ordinal after MIDI or tempo edits, and
releases removed or no-longer-current onsets. Loop releases use the running addresses. No output transport or voice tracking allocates, frees, locks,
logs or dispatches from the audio path.

The app-side consumer belongs in `EngineApp::tick()` after engine main servicing:

1. Drain the take API and publish cumulative event/voice drop counts in
   `engine.status` and `engine.stats`. On a changed drop count, close incomplete
   gestures and resynchronize parameter values on main; do not invent missing
   automation points. Reject removed FX, superseded plan/transport epochs and
   records whose patch sequence is not the engine's current one (produced
   before a patch-only edit, or while it applied, ADR-0092).
   For anticipative records, use `streamSample`/`playedStream(epoch)` to delay
   timeline recording until that stream position has played (discard abandoned
   seeks). Idle records apply immediately.
2. Validate the parameter against the current instance. Global ParamValue
   records (all address fields `-1`) update `FxInstance::params`; per-note values
   retain their address and do not overwrite a global default. VST3 values are
   normalized: call `Instance::plainParam` on main before storing them. Semantic
   CLAP values also update the matching patch key using core-params' conversion
   to physical units. Preserve flags: `DontRecord` prevents envelope recording,
   while the current value may still be reflected in the model/UI. Outputs for
   read-only parameters (meters, readouts) are display values: they never
   enter the model, history or automation.
3. GestureBegin starts a pending model edit keyed by FX and parameter; values
   update that pending edit; GestureEnd commits one undo version. Do not invoke
   `fx.param.set`/`Instance::setParam` for these values. For semantic patch changes,
   call `InstancePool::acknowledgeOutputPatch` with the updated patch before a
   head-change rebuild, so `get()` does not echo it through `setPatch`. Only
   plug-in-originated parameter changes may use this acknowledgement; other
   edits still follow normal validation and delivery. Plain outputs capture the
   current opaque state and acknowledge its hash through
   `InstancePool::acknowledgeOutputState`; numeric values remain display/diff
   fields. Replay logs retain the resolved value edit and recorded envelope
   points without executing a setter during output consumption. The consumer
   bounds pending records (8192), gestures (128) and recording points per gesture
   (4096); overflow discards missing recording intervals and resynchronizes
   current values: the writable parameters of the FX whose outputs were
   dropped (all FX when the engine's 512-entry table of them overflows or a
   voice drop names none; read-only outputs never enter the model), and a
   semantic FX's resynchronized value is an edit only when its patch changes.
   Status exposes `dropped`, `voice_dropped`, and
   `failed_errors` alongside legacy `failed_fx` IDs. Failure entries and
   `fx.failed` retain `reason` and add structured `error` details.
   Known limit: a plain plug-in's state is one opaque blob, so when two of its
   parameters have overlapping gestures (an XY pad), the step committed for the
   first also captures the second's in-progress value.

Bridge integration carries matching additions to the `Op::Service` response in
`host/clap-host/bridge/bridge_main.cpp` and `src/bridge_client.cpp`: carry the
process-request bit and an ordered list of `{param id, clear flags}` after
`paramsRescanFlags`, update both reader and writer together, and expose the
received wake bit to the proxy's RT-safe wake API. Both endpoints use the
unreleased protocol v6 layout (ADR-0013).

### Plug-in catalogue scan reports (2026-10-04)

`plugin.list` and `plugin.scan` return `scan` beside `plugins`, including an
empty catalogue. Its `failures` entries contain `{module, reason: {code, message,
path, hint, details?}, cached}`, `warnings` contains structured reasons, and
`cached_modules` counts reused module results. EngineApp retains this report
with its catalogue so unrelated scans cannot overwrite it. Filtering affects
only available plug-ins; explicit scanning replaces the catalogue/report pair.
`plugin.scan {wait: false}` returns at once with the current catalogue and a
`job`: a worker scans every module through the supervised scanner (stock ones
too, since nothing may load in that thread; without the bridge the job fails
with `host.scan.unsupervised`), and the model thread installs the new pair
when the job is done. A background scan still running is returned instead of
a second one. Modules in the stock folder are never served from the scan
cache (2026-10-12): the script player's plugins are the scripts in its
library (ADR-0032), which its file says nothing about. Nor is a script
player or player export anywhere else (2026-10-14): a module whose file
holds the player's mark (`SUPPLE_SCRIPT_PLAYER_MARK`) gets no cache record,
and records written before then (without `version: 2`) are ignored.

### Initial engine-start failure (2026-10-08)

A failed initial plan load stops and releases the engine/backend before
`EngineApp::startEngine` returns its error, including with a caller-supplied
backend. A corrected session can start again without an intervening stop.


## Optional host-process controls (2026-10-08)

`EngineApp::setMemoryLock(bool)` changes an app-wide, default-off request.
Loaded Preferences apply before startup; setters apply immediately on the
model thread while running or store intent while stopped. After a successful
backend start, numeric validation and initial plan load, the app acquires its
requested controls. Initial allocation/setup is not protected. An acquisition
failure warns but does not prevent usable audio from starting. There is no
retry loop; the next successful start or explicit re-enable retries.
`haltEngine()` stops the backend and processing workers before releasing the
controls. Project replacement/device recovery release and reapply them.
Transport stop/pause keeps them: the engine still processes monitored inputs.
The helper destructor also releases acquired controls. No control operation
runs on an audio path, and offline rendering acquires no separate request.

### Memory locking

Use `mlockall(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT)` with no fallback.
CURRENT covers existing mappings and FUTURE covers later in-process plug-ins,
sample libraries and heap/stack mappings. ONFAULT locks resident pages and
marks untouched pages for locking when populated instead of committing
untouched stacks/arenas. It does not prefault memory or eliminate cold-page
faults. The request covers the entire host process, including GUI/media and
in-process plug-ins; shared/dedicated bridges and scanner processes are
excluded. Host-side shared mappings do not protect bridge heaps/stacks.

Read `RLIMIT_MEMLOCK` for failure diagnostics without changing it. ENOMEM and
EPERM include the account's allowance and suggest disabling the option,
reducing mapped memory or asking an administrator to review the allowance;
EINVAL reports unsupported flags. Unlimited allowance is not unlimited RAM.
Future allocation or stack expansion can fail under the policy; stack failure
can terminate the process. Large sample libraries may fail to load.

Disabling/stopping calls `munlockall` only after our successful acquisition.
It also removes locks made by in-process plug-ins/libraries. Locks do not
stack, and prior independent lock ownership cannot be restored: opting in
accepts this process-wide cleanup behavior. Default-off and failed acquisition
never unlock another plug-in's locks. A failed unlock retains active ownership
and its error for a later cleanup attempt. In-process third-party locking
calls can alter the policy; status reports our last successful operation.

### Status and test seam

`engine.start`/`engine.status` add `memory_lock`: `{requested, active, error?}`.
The optional typed error has `code`, `message`, `path`, `hint`, and numeric
`errno`. Codes are `engine.memory_lock`/`engine.memory_unlock`, path
`/lock_memory`. Intent and cleanup errors remain visible while stopped;
requested=true/active=false without an error means pending while stopped.
Failures also appear in `warnings` alongside existing engine warnings.
AppOptions may inject `RuntimeProtectionOps` for deterministic lifecycle/error
coverage. Default wrappers call Linux libc; these operations are model-thread
only. Tests fake memory locking instead of locking the runner's address space.


### CPU idle latency (2026-10-08)

`EngineApp::setPreventDeepIdle(bool)` changes the separate default-off
request. Open `/dev/cpu_dma_latency` with `O_WRONLY | O_CLOEXEC`, write one
native `int32_t` of `kCpuIdleLatencyUs = 20` microseconds and retain the fd
while enabled and the engine runs. C1-class exits are typically a few µs;
deep states take roughly 100 µs to over 1 ms, a substantial part of a
64-frame period. This fixed small threshold permits shallow idle instead
of the zero-latency POLL-only constraint on the measured workstation.
There is no runtime sysfs/topology read to choose the value, frequency lock
or guarantee of a particular C-state residency on arbitrary hardware.

The request affects system idle while held and may increase power, heat and
fan activity. Steppe offers this owned, close-scoped QoS request instead of
locking CPU frequency: governor/EPP changes are privileged and system-wide.
The request leaves frequency policy alone and never escalates privileges.

Retry EINTR on open/write; a zero/short write fails with EIO and is never
completed with partial integer writes. Every failed write closes its fd.
Disable, stop, project/device restart and destruction close the acquired
fd exactly once. Closing removes only this request, without reading or
restoring a remembered global QoS value. On Linux close is not retried even
on EINTR: the descriptor is already released and may have been reused.
CLOEXEC excludes child bridge/scanner processes from retaining the fd.

`engine.start`/`engine.status` add `cpu_idle_latency` with the same typed
`{requested, active, error?}` structure as memory_lock, plus `automatic`
(below). Failures use
`engine.cpu_idle_latency`, path `/prevent_deep_idle`, with numeric errno and
a corrective hint. EACCES/EPERM explain that the account needs membership
in a group with device write access (commonly audio; policy varies);
ENOENT/ENODEV report that the device is unavailable. Acquisition failure
keeps intent enabled and audio usable; close failure records an error with
active=false because the descriptor has been released.

**Automatic hold (2026-11-16, ADR-0086).** On Linux the request is also held,
without the option, while the device callback processes a bridged plug-in:
`Engine::bridgedLive()` (main thread) is true when the last published plan's
callback instances (its `liveInstances` with the renderer, all of
`instances` with the in-callback executor) include an isolated one, and
false when stopped. `EngineApp::tick` passes it to
`RuntimeProtection::setBridgedLive`, which acts on a change only, so a
refused request is not retried every tick. The request is held while the
option or the automatic state wants it, and released when neither does or
the engine stops. Status gains `automatic`. An automatic failure keeps its
error, with a hint that does not mention an option, but adds no engine
warning; it is cleared when the automatic state ends and the option is off.
On macOS and Windows `setBridgedLive` does nothing: they have no
per-process request (ADR-0086).

Tests inject open/write/close operations for all lifecycle and error paths.
The real libc write/close path is exercised against a temporary file through
an injected open. Automated validation never writes to the real device.
User-only acceptance: enable/disable and engine-stop/restart/exit cleanup
with the real device, plus repeated sparse/dense 64/128-frame sessions,
actual granted period, callback tails/xruns and power/thermal/fan behavior.
No latency benefit is claimed without those measurements.

### Audio-thread page faults (2026-11-15, ADR-0084)

A page fault on a deadline thread (the callback, a live helper) is cheap
until it has to wait for the process's address-space lock, which has no
priority inheritance. Three rules keep the audio threads from faulting where
the app can prevent it:

- **Transparent huge pages (Linux).** While the engine runs, the process is
  out of THP: `prctl(PR_SET_THP_DISABLE, 1, PR_THP_DISABLE_EXCEPT_ADVISED)`,
  so regions a plug-in advised with `MADV_HUGEPAGE` keep huge pages, or on
  kernels before 6.18 (which refuse the flag with EINVAL)
  `prctl(PR_SET_THP_DISABLE, 1)`, and none do. This is a per-process setting
  that changes no system setting; children inherit it, and every bridge
  process sets it at its own start too, since a bridge may start before the
  engine. Without it, with `transparent_hugepage=always`, khugepaged
  collapses the heap's small pages into huge ones every scan (10 s by
  default); an audio thread touching a range being collapsed faults and
  waits for khugepaged, a nice-19 kernel thread that a loaded machine starves
  of CPU, to finish. Memory locking does not prevent this: the kernel
  collapses locked pages too. Applied after the backend start with the other
  controls; a refusal is reported and the engine runs regardless. The
  process's mode is read first (`PR_GET_THP_DISABLE`: 0 none, 1 all, 3 all
  but advised regions). A mode the process already has, from whatever
  launched it or a plug-in, is kept as it is, whether stricter (1) or the
  same (3), reported as `inherited` and not undone at stop. Only from 0 does
  the engine set it, and at stop it writes 0 again only if the mode is still
  the one it set; a refused release, or a refused mode read at stop, is
  reported, and the engine still owns
  it until a later stop releases it. A bridge likewise sets it only from 0.
  `engine.status.thp_opt_out` is `{active, advised_only, inherited, error?}`
  (code `engine.thp_opt_out`, path `/thp_opt_out`); never active on macOS or
  Windows, which collapse no pages under a running process.
- **Memory is touched when it is prepared.** Storage the audio path will use
  is written when it is allocated, off the audio thread, so its first touch
  is not a callback's. Engine buffers are value-initialised containers. The
  script runtime's `Buffer` allocates with `calloc` and then writes one byte
  per 4 KiB, since a large `calloc` returns fresh pages it does not write.
- **Locking stays optional** (Memory locking above): it keeps pages from
  reclaim and, where the kernel's `compact_unevictable_allowed` is 0, from
  compaction, but it prefaults nothing.

`engine.status.audio_page_faults` counts the faults since the engine
started: `{scope, minor, major, threads}`. On Linux, `scope` is
`audio_threads`: the callback thread's and the live helpers', each counted
from when the main thread first sees it (the helpers at start, the callback
within a tick of its first cycle; the cycle records `pthread_self()`, which
makes no system call). The kernel thread id comes from the thread's CPU clock
id and the counts from `/proc/self/task/<tid>/stat`. On macOS (`task_info` event counts:
`getrusage` there misses fresh pages' zero-fill faults) and Windows (`GetProcessMemoryInfo`, which does not separate minor from major,
so `major` is omitted) neither keeps a count per thread, and `scope` is
`process`. `none` while stopped. The bench reports the same count over its
measured interval, and each thread's on Linux (`tools/bench/README.md`). On macOS a counter at its 32-bit limit makes `saturated` true, with
`minor` 0 and `major` omitted: the counts are not counts then.

### Headless platform mechanisms (2026-10-14, ADR-0035)

`EngineApp::serviceWaitable()` borrows its relay's typed source; `serviceFd()`
remains POSIX compatibility (-1 on Windows). Headless loops/MCP wait on the typed
source. Dummy/offline scheduling stays normal. Linux FIFO/affinity/rlimit behavior
is unchanged. Windows High is a verified thread priority, macOS High a self-applied
QoS class; neither promises real-time callback scheduling. Backend helper setup
remains mandatory for real-time live multiprocessing. Process memory locking with
future/on-fault semantics and the CPU latency-device preference report unsupported
on macOS/Windows when requested; defaults remain off. RuntimeProtectionOps exposes
portable MemoryLimit/ProtectionWriteSize types, aliased to rlimit/ssize_t on Linux.
Chain/template/work-directory paths use core-base's platform roots.

Media cache identities use Linux/macOS stat fields or Windows volume/file ID,
size and last-write time; content hashes take precedence on every platform.
Preset/media publication never replaces an existing destination unless explicitly
requested. Linux retains reflink and hard-link publication; macOS copies to a
private temporary file before linking; Windows uses a non-replacing same-directory
MoveFileEx. Cache identity strings are internal and are not serialized.

### Native hosting boundary (2026-10-14, ADR-0036)

macOS and Windows enable the engine library and CLI with native CLAP/VST3 hosting,
dummy audio and offline rendering. The platform's RT output must equal its
offline output, including the anticipative renderer. A native fixture test
runs this contract through both executors, Console 9, latency compensation and
multiple worker capacities. JACK/fake JACK and the hardware benchmark stay
Linux-only; an explicit JACK request returns backend.unsupported,
path /backend and a hint to select dummy. Both native platforms also enable
sandbox rendering through native audio-file I/O. Shared/dedicated bridge creation
uses the real native bridge through the same InstancePool load boundary; it
never substitutes a local instance. Native tests compare both isolation modes
through render-ahead and offline execution on macOS and Windows. Portable fault
fixtures verify shared-process failure propagation, dedicated-instance survival,
structured errors and restart ownership through InstancePool after crashes and
expired process deadlines.

`engine.start` defaults to JACK on Linux and dummy on other platforms. Its
registry documents `backend.unsupported` and the default difference, so the CLI's
play command also works with no backend flag on the native headless port.
Background jobs and render cancellation use core-base StopToken/JoiningThread;
Linux uses the standard types, while Apple uses the non-RT fallback in core-base.

### Native audio files (2026-10-14)

The audio-file library, engine/CLI and sandbox build on Windows as well as
Linux/macOS. Recording creation is exclusive: an existing take or link is
never replaced. Recovery opens the final path without following links; Windows
rejects reparse points and directories on the opened handle. Windows opens use
CreateFileW and permit read/write/delete sharing. Audio-file error messages
convert native paths explicitly to UTF-8, avoiding Windows ANSI conversion
exceptions for Unicode take names. Inspection and recovery can
coexist with a flushed writer. Recording and recovery positions are 64-bit on
all platforms (Windows long is only 32-bit). Recording's existing RIFF limit
and complete-frame recovery rule are unchanged; no file operations run on audio
threads.

RF64 rendering streams into an exclusively created sibling temporary file,
flushes its contents to disk, closes it, and atomically replaces the destination.
POSIX syncs the containing directory; Windows uses MoveFileExW with replacement
and write-through. External Windows readers denying delete sharing can prevent
publication; failure removes only our own temporary file and returns audio.write
with a corrective hint. Small renders retain core-base writeFileAtomic.

### Native device backend (2026-10-14, ADR-0038)

`PortAudioBackend` implements Backend on macOS/Windows. `start()` accepts the
existing BackendConfig rate/block/channels; PortAudioOptions selects exact host
API and input/output names (empty: defaults). `driverDevices()` uses its injected
API for tests; static `devices()` uses the native API. Library initialization
and teardown stay off RT. Failed opens release every acquired stream/reference.
The Linux build compiles the driver-independent wrapper for fake-driver tests,
but never builds or initializes the PortAudio library.

`engine.start` accepts backend `jack|portaudio|dummy` and optional `host_api`,
`input_device`, `output_device`, `rate` (1..768000), `inputs` (0..64) and `outputs`
(2..64). Native stream audio inputs default to zero, outputs to two (the
GUI asks for every output of the chosen device, up to 64); JACK registers one
output per physical playback port, 2 to 64, unless `outputs` is given. The
engine writes every opened output channel (Device outputs, §2.3). Both
native and JACK backends also clear any additional opened output channels to
silence before invoking the engine. `engine.inputs` lists `outputs`, one
`{channel, port}` per opened output (JACK: the playback port it connects to;
native: empty).
engine.start enables native MIDI inputs (ADR-0042). JACK keeps its capture/MIDI behavior. Native default backend stays
dummy (the GUI starts the saved device instead, ADR-0050); Linux's existing
default stays JACK. An explicit `rate` sets `PortAudioOptions::changeDeviceRate`:
CoreAudio switches the device's nominal rate (`paMacCoreChangeDeviceParameters`,
converting what it cannot run). WASAPI shared mode always requests Windows'
rate conversion (`paWinWasapiAutoConvert`). These go through the optional
`PortAudioApi::streamInfo` hook (null in the fake driver), for both stream
directions. The CLI play command forwards these
fields through --host-api, --input-device, --output-device, --rate, --inputs and
--outputs. Device recovery retains them alongside block/anticipative settings.

`engine.devices {backend: portaudio}` (the default) returns `devices` containing
`host_api`, `name`, `inputs`, `outputs`, `default_rate`, `default_input` and
`default_output`. It enumerates without opening a stream.
The result schema declares these required fields: host API/name strings,
integer input/output capacities, a numeric default rate, and boolean default
input/output indicators. `{backend: dummy}`
returns an empty list without library initialization. Linux portaudio requests
return backend.unsupported, path /backend and a JACK/dummy hint; JACK device
queries direct the caller to engine.inputs. A failed start carries its cause
in the code, set where the backend holds the PortAudio error number and host API
(or the JACK client status), so callers switch on the code, never the message
(the message stays the driver's text), each with a corrective hint:

| Code | Cause |
|---|---|
| `backend.host_api.not_found` | no host API by that name (`/host_api`) |
| `backend.device.not_found` | no device by that name, or no default (`/input_device`, `/output_device`) |
| `backend.device.ambiguous` | two devices of the host API share the name |
| `backend.device.no_inputs` | the input device is listed but has no inputs (an output-only device asked to record) |
| `backend.device.channels` | fewer channels than asked (`/inputs`, `/outputs`) |
| `backend.device.invalid` | `paInvalidDevice`: the device is gone, or (WASAPI) held in exclusive mode by another app |
| `backend.busy` | `paDeviceUnavailable`: held by another stream of this process (ASIO), or taken over by another app (Core Audio hog mode) |
| `backend.driver` | the driver or server did not start: on ASIO `paUnanticipatedHostError` (the driver failed to load or `ASIOInit`, as when another app holds a single-client driver); on JACK, no server answered |
| `backend.rate` | `paInvalidSampleRate` only: the device does not run at the rate (`/rate`); on ASIO also an interface that is not present (`ASE_NotPresent` maps to it), which the engine cannot tell apart. An omitted rate is the project's, so the hint names engine.devices' `default_rate` |
| `backend.unavailable` | the JACK library is missing or lacks the JACK API |
| `backend.config` | an invalid rate, block or channel request, or an invalid stream rate returned |
| `backend.open` | any other failure (another PortAudio error, port registration or activation on JACK, the device monitor) |

Duplicate matching names are rejected. No numeric device index is persisted across restarts.

Callbacks use non-interleaved float32 and paClipOff/paDitherOff. They call the
engine in chunks no larger than blockSize; period reports the complete device
callback size. Status exposes device input/output latency in frames, affected
callback xrun counts, and restart_needed/reason for loss or changed rate.
Period changes need no restart. A native device monitor observes state every
250 ms and wakes the app only for a restart transition. The monitor alone queries
native driver rates, including the initial observed baseline, and publishes
rate/loss flags through atomics. Owner-side status reads those flags and cheap
PortAudio stream info/activity; it never calls the native driver rate query.
Normal stop supplies a
final zero-frame callback on the device thread; a lost/stalled device closes and
joins before owner-thread lifecycle fallback, as with JACK. All native threads
and device-library initialization references are released on stop/destruction.

Backend's optional nativeThreadPolicy/prepareCurrentThread/releaseCurrentThread
hooks apply native policy on each owned thread before work and undo it at exit.
Native prepareLiveThread refuses cross-thread setup rather than silently
accepting normal-priority helpers; injected normal drivers retain normal setup.
Each helper class's non-RT startup barrier verifies all results before enabling
it, rolling back the class on refusal. Live setup failure disables that pool;
background/renderer failures warn and report the actual fallback class. Windows
uses MMCSS Pro Audio, macOS device-derived Mach time constraints (ADR-0038),
reported as high/priority 0. On macOS the live helpers (not the renderer or the
background pool, which work ahead of the device cycle) also join the opened
device's CoreAudio IO workgroup (`kAudioDevicePropertyIOThreadOSWorkgroup` of
the stream's output device) from their own thread, after the time-constraint
policy and before receiving jobs, and leave when they exit (stop, restart or
device change all end the helpers). A refused lookup or join never fails the
start: the helper keeps its time-constraint policy and `engine.status.workgroup`
reports `state` (`none`, `ready`, `joined`, `partial`, `failed`), the `joined`
and `failed` helper counts and the last refusal's `os_error`, `message` and
`hint`; `partial` and `failed` also add a start warning. The per-block path
makes no workgroup call. The native callback reports none because PortAudio
does not expose its thread policy; the OS/host API owns it. These are distinct from core-base's generic High
QoS/priority helper. Offline and dummy threads stay normal; background Normal opts out. Native
background elevation also requires leaving two usable CPUs free, as on Linux;
an oversized explicit pool stays normal with a corrective warning.

The fake API tests cover device selection, odd-period chunks, both executors'
exact offline equivalence, PDC/Console 9, several worker counts, failures and
final callback ownership. Real native device tests are skipped unless
SUPPLE_AUDIO_DEVICE_TEST equals `1`; default CI never opens a stream.

Native CoreAudio may perform host-side rate conversion when its nominal hardware
rate differs from the requested stream rate. Engine config reports the stream
rate; the monitor separately observes nominal hardware changes and requests a
restart. Hardware output conversion is outside the engine's exact-output
contract, which is verified at the float callback boundary.

### Native MIDI inputs (2026-10-15, ADR-0042)

PortAudioBackend accepts BackendConfig.midiInputs and opens up to 16 native
sources: CoreMIDI protocol-1 channel messages on macOS, WinMM CALLBACK_FUNCTION
on Windows. `engine.start` enables these inputs without adding arguments;
`engine.inputs` lists their display/device names while running, empty while
stopped. Existing midiPortMask/routeMidi filtering, rechanneling, monitoring,
MIDI learn and recording use the same Backend MIDI interface as JACK.
The injected PortAudio constructor accepts an owned detail::MidiSource for
device-free tests; audio-only injected drivers return empty MIDI lists.

OS receive threads produce timestamped short channel messages through one
preallocated SPSC ring (2048 entries) per port, with a single producer per
source. Audio snapshots and drains each ring, accepting at most
kMaxMidiInputEvents (1024) per device period in port-index/arrival order before
merging. Monotonic per-port runs merge in O(n * k), k <= 16; a nonmonotonic run
uses a bounded stable insertion-sort fallback on that port only. Equal frames
retain port-index order, then arrival order within a port. Preallocated chunk
storage doubles as merge scratch; no new allocation. Events rebase into each
engine chunk, including the final short chunk. No allocation, lock, wait or
logging on receive/audio paths. `Backend::midiDropped()` defaults to zero;
PortAudio and JACK expose rejected messages and ring/period overflow
(JACK: a message that is neither a channel nor a sync message, or past a
period's `kMaxMidiInputEvents` / `kMaxSyncMidiEvents`) through it and
`engine.status.midi_dropped`. Sync messages (Clock, Start, Continue, Stop,
Song Position, MTC quarter frames and the MTC full-frame SysEx) go to
`Backend::syncMidi()` instead (§ External sync); other system messages and
SysEx are dropped and counted per reported packet/buffer; driver-unreported
SysEx loss cannot be counted.
Two fixed WinMM long buffers are recycled on the monitor, never inside its
MIDI callback. Short-message reception does not depend on recycling them.

CoreMIDI host ticks use mach_timebase_info; WinMM milliseconds since start
use an immediately preceding QPC anchor, with nearest-epoch 32-bit wrap
reconstruction. At callback entry, timestamps in the preceding audio period
[end - frames/rate, end] map to [0, frames-1], rounded to nearest frame and
clamped at endpoints. This preserves received spacing with up to one period
of delivery delay; it is not sample-accurate hardware/ADC synchronization.
WinMM has millisecond resolution. Existing recorder capture-latency compensation
still applies. Names/indices remain fixed while running. The native monitor
observes source topology changes and publishes `engine.status.midi_devices_changed`
(true until stop/start), with midi_devices_message "MIDI devices changed; restart
the engine to pick up the new set". This advisory does not set restart_needed,
stop playback/recording or restart the engine. Removed inputs go silent; opened
ports/names remain fixed. Only an explicit user/LLM engine stop/start adopts the
new set. The monitor keeps checking audio rate/loss and recycling WinMM buffers
for surviving inputs after this advisory. Identical WinMM
device replacements with indistinguishable caps/names may not be detected.

No sources is success. Each refusing input is closed and skipped without
preventing audio startup or other MIDI inputs from opening. `engine.inputs`
and midiPorts() contain only opened inputs, with compact indices fixed before
audio starts. `engine.start`/`engine.status.midi_warnings` reports each skipped
input as {device, os_error, message, hint}; os_error is the numeric OS result,
and hint is "close the other application and restart the engine". A text copy
also appears in warnings. Source-wide setup failures are warnings too; audio
startup fails only for audio reasons.
Stop joins the monitor/audio callback before resetting/disposal of MIDI and
queue destruction. Linux keeps JACK and builds only device-free shared logic.
Real-device tests require SUPPLE_MIDI_DEVICE_TEST exactly 1; they use fake
audio and never open an audio device. Native SDK tests do not establish physical
input timing, disconnect reliability or performance on every driver.

### External sync (ADR-0071)

**Device input.** `Backend::syncMidi()` holds each callback's sync
messages from every MIDI port, sorted by frame: F8, FA, FB, FC, F2 lsb
msb, F1 dd and F0 7F id 01 01 hh mm ss ff F7 (`SyncMidiEvent`, up to ten
bytes, at most `kMaxSyncMidiEvents` = 512; more are counted in
`midi_dropped`). `syncMidiSize()` classifies them. JACK takes them from its
events; the native buffer keeps a 512-entry queue per port (`pushSync`,
`syncChunk`) beside the channel queues; CoreMIDI sends UMP type 1 through
`push` and reassembles SysEx7 (start/continue/end or complete) into the
full frame, keeping no other SysEx; WinMM's MIM_DATA system messages go
through `push` and a ten-byte full frame in a long buffer through
`pushSync`. `midiInput()` is unchanged. The dummy backend's `sync` option
scripts them for tests.

**Settings and status.** `EngineApp` holds `SyncSettings` (`sync.get`,
`sync.set`; ADR-0071 §2) and resolves them for the engine: the MIDI port by
exact name, else the first containing it (case-insensitive), none (with a
warning) while that input is disabled in its MIDI port record (ADR-0072;
its `offset_ms` moves played and recorded MIDI, not the chase, whose
latency is §9's and whose offset is `sync.set`'s); the input and
output channels against the device; the project's timecode; the freewheel
time from its frames. Unresolvable members become warnings in
`engine.status.sync.warnings` and `sync.set`'s warnings, as does an LTC
output channel a master's `hw_output` plays to. An LTC output channel the
device lacks uses its last output instead, with a warning. The owner-thread
`EngineApp::resolvedLtcOutputChannel()` shares that resolution with the GUI
(1-based, zero when LTC is off or no engine/output is available); the setting is
kept. `engine.status.sync`:
`{source, input, present, running, locked, state, bpm, timecode, rate,
chase_rate, error_ms, locate_ms, ltc_output, warnings}`.

**The callback.** `Engine::setSync(SyncConfig)` builds a `SyncRuntime` on
the main thread (libltc's decoder and encoder are allocated there) and
publishes it; the callback adopts the newest at the start of a cycle, and
`service()` frees those before the adopted one. After each cycle's
transport work (`cycleTransport`, count-in included) `runSync` gives the
runtime the cycle's sync messages, inputs and outputs, its device frames
(counted from start), the timeline it played (start and end, from the
rate clock), the plan's and the device's output latency and the device's
input latency, and the plan's time map. The runtime feeds the configured
port's messages to the clock or MTC receiver (ADR-0070 §3-4), or the
configured input channel to the LTC decoder at device frame − input
latency; gives the tracker each observation and the receivers' jumps; and,
while the source runs, updates the tracker with the heard local position
(processed timeline − output latency × speed) in the source's units: beats
for MIDI clock (the plan's time map; nominal speed bpm/60 there), timecode
seconds (timeline + project start + offset; nominal 1) for MTC and LTC. A
loss is final when nothing was observed for the freewheel time (0: at the
receiver's timeout); the tracker then resets. The chase rate is the
tracker's while the transport plays and the source runs (MIDI clock only
with follow tempo), held through a freewheel, else 1. The first clock after
Start or Continue has no tempo yet: its position is counted on at the
project's tempo. LTC output renders into its output channel after the
masters, labelled with what leaves with it: the runtime keeps the timeline
each cycle processed as spans (device frame, start, seconds per frame; two
at a loop wrap, split where the loop's end was reached; a ring of 4096) and
writes output frame d from the span covering d − the plan's latency, so LTC
names the audio it is heard with (the device's own latency delays both;
so does the hardware MIDI lead's audio history, which runs after LTC is
written, so the lead is not part of that latency).
Frames before the oldest span, or processed while stopped, are silent; a
start, locate or wrap (a step of more than a frame) starts the encoder
again at the next frame boundary; stopped, it is silent at once. The status is published
under a sequence lock (`Engine::syncStatus()`); the main thread is woken on
a change of presence, running, lock, a start, stop, jump, relocate request,
final loss, or a chase rate change (at most every 10 ms). Everything here
is `SUPPLE_RT`.

**The main thread.** `EngineApp::tick` re-resolves the settings (a no-op
when unchanged) and acts on the status (ADR-0071 §4): MIDI clock: Stop
pauses; running after Start/Continue (the first clock) seeks to its beat
plus (the measured seek time + the heard latency) × tempo / 60 (the
master's tempo, the project's before a second clock), − offset, and plays;
a Song Position change while stopped seeks there;
with follow tempo a relocate request seeks to the prediction. MTC/LTC:
running while stopped seeks to the prediction (timecode − start − offset at
the heard time) plus the measured seek time × the master's speed, and
plays; a relocate request seeks likewise; running backwards pauses; a jump
(full frame) while stopped seeks. A final loss pauses. Then
`Engine::setSyncRate(chase rate)` while playing, 1 otherwise.
The measured seek time (`locate_ms`) averages how long these seeks took on
the steady clock, each new one weighing half, clamped to 0-500 ms and
starting at 50 ms. `AppOptions::syncLocateSeconds` fixes it instead, for a
device whose time stands still while the main thread seeks: the dummy
backend's manual mode, in tests, which must not pass or fail by the wall
clock.

**The sync plan and the chase scale.** While `SyncConfig::listens()` (an
input source) `load()` compiles with `SyncOverrides`: the plan is
`syncPlan`, and on an anticipative engine every node is live. When
`SyncConfig::chases()` too (MTC, LTC, or MIDI clock with follow tempo) it
is also `syncRate`: `rateEnabled`, `masterPlayrate.preservePitch`, clips'
stretch converters even at 1.0, `masterPlayrate.rate` = saved × scale.
Stretch at 1.0 is not a null: a chasing session's audio clips differ from
unsynced playback even when locked (a 441 Hz sine by about −25 dB of its
level in the tests); MIDI clock without follow tempo and LTC output alone
play the same samples as without sync. LTC output alone compiles no sync
plan. A change of `listens()` or `chases()` reloads; `setSync` resets the
scale to 1 when nothing chases. `setSyncRate(scale)` (0.25..4) publishes saved × scale as a scalar
control through `loadScalarRate` (ADR-0046: ramp, 150 ms low-pass, both
executors); on a plan without renderer nodes the control lands after the
consumed position plus headroom, not the render-ahead. The saved rate and
`transport.playrate.get`'s `requested_rate` never change; `setSyncRate` does
nothing on a plan that is not `syncRate`. `startRecording`
skips the queued-trajectory check on `syncRate` plans (the saved-rate check
stays), and its recorder is drift-tolerant there: a block whose timeline
position is within a block of the expected one continues the run (the take
drifts by the chase rate's distance from 1), only a larger jump starts a new
run. Offline renders compile without overrides.

### Native MIDI outputs (ADR-0069)

`BackendConfig.midiOutputs` opens up to 16 destinations; `engine.start` enables
it. `Backend::midiOutputPorts()` names them, fixed while running;
`engine.inputs.midi_outputs` lists the same names (empty while stopped).
Clock and MTC generation is § MIDI clock and MTC output. `writeMidi(MidiOutputEvent)` is
callback-only: 0-based port, frame within that engine chunk, size 1..10 and ten
inline bytes. Messages must be complete MIDI 1.0, including system realtime,
SPP, quarter-frame MTC and full-frame MTC SysEx. Running status and SysEx longer
than ten bytes are rejected. The default Backend has no destinations.

One fixed 1024-event budget spans the entire device period, even when audio
splits it into chunks. Events are stably sorted by period frame (ties retain
write order). `writeMidi` returns false on immediate rejection; accepted output
can still be lost downstream. `midiOutDropped()` / `engine.status.midi_out_dropped`
counts validation/budget, queue and device-write rejection, backwards per-port
timestamps and queued events discarded at stop. Counters reset on start.
OS-accepted output cancelled at stop is not counted. DummyBackend can declare
ports/latencies and captures bounded output with absolute frames since start.
Read its capture only after manual run/stop, as for audio capture.

`midiOutputLatency()` / `engine.start/status.midi_output_latency` is a signed
frame array parallel to the port names: **audio output latency minus MIDI port
latency**. Positive L means an event at cycle frame f leaves MIDI L frames before
audio at f reaches the speakers. Future routing must delay MIDI by L plus the
user's signed offset; negative values require advance/lookahead. The backend
never applies that alignment itself. MIDI latency refers to the first byte;
external instrument processing latency still needs user calibration. JACK
subtracts each MIDI port's maximum playback latency from the first audio port's
maximum; optional missing latency reporting means zero. CoreMIDI/WinMM currently
assume zero MIDI wire latency, so the delta is the audio output latency estimate.

JACK registers midi_out_N connected to physical MIDI playback ports, clears
all buffers every period and writes sorted events at their frames. Native
audio publishes to one preallocated 2048-event SPSC queue per destination. Its
frame zero in the sink's clock is callback entry time plus PortAudio's
`outputBufferDacTime - currentTime - audioOutputLatency`; f adds f/rate, with
chunk offsets applied once. With no callback time info, frame zero is entry
time. Source clocks are Mach host time on macOS and QPC on Windows.

CoreMIDI shares the input client and sends protocol-1 UMP event lists with host
timestamps from a sender thread. Apple promises future timestamp scheduling,
but does not document a realtime-safe send call. System messages use system
UMPs and bounded SysEx uses SysEx7 UMPs. WinMM's sender uses MMCSS Pro Audio
(or highest thread priority), a high-resolution waitable timer and
midiOutShortMsg/midiOutLongMsg, all off audio. Eight preprepared long buffers
per destination bound in-flight SysEx. Expected WinMM jitter is roughly 1 ms;
no hardware timing measurement is claimed. There is no timeBeginPeriod or
system-wide configuration change. Both senders poll within 1 ms; CoreMIDI
submits future events to its scheduler immediately, WinMM sends when due. Late
events are sent late; future routing must provide scheduling lead time.

Refusing outputs are skipped, audio remains running, and
`engine.start/status.midi_output_warnings` contains device, numeric os_error,
message and hint (also copied into warnings). Names contain only successfully
opened/connected destinations. Topology changes latch midi_devices_changed
without automatic restart. JACK's optional registration callback conservatively
includes audio port topology. Native output monitoring uses the existing device
monitor. Stop first emits held-note releases from the final audio callback, joins audio,
then submits queued releases with sink timestamps of now and joins the sender. Other
future queued events are discarded. CoreMIDI never flushes endpoints; submitted
events drain and only the owned port/client is disposed (ADR-0072). WinMM
resets, unprepares buffers and closes owned devices; reset can send all-notes-off.

Device-free fake clocks exercise the production queue path, including scheduling,
loss and shutdown. `SUPPLE_MIDI_DEVICE_TEST=1` alone enables native device opens
with fake audio and no explicit MIDI sends. macOS/Windows native CI and physical
timing/disconnect validation remain pending. No additional SDK/library is linked.

### MIDI clock and MTC output (ADR-0073)

**Settings.** Clock output reads the same per-port record as hardware MIDI
routing (`MidiPortPreferences`, `midi_ports.h`; see "Hardware MIDI routing
(ADR-0072)"): routing owns `enabled`, `offset_ms` and `reset_on_stop`, and
the record adds `clock`, `transport`, `spp`, `mtc` and
`clock_while_stopped` (booleans, outputs only, validated by
`parseMidiPortPreferences`; `MidiPortSettings` carries them). The records
live in `preferences.json` (`supple.preferences/1.8`) as `midi_outputs`
`{"<port>": {...}}`; `EngineApp` is their only writer (`midi.outputs.set`),
and a malformed record is skipped at startup with a warning, as for
routing. `midi.outputs.set` resolves its port by a unique case-insensitive
match among present and recorded names (a port that is neither is refused,
as for routing: a record is made while its device is connected), applies
the edits, drops a record back at every default, then rebuilds the plan and
`applySync()`. `syncConfigFor` resolves each output
of `Engine::midiOutputPorts()` by its exact name into
`SyncConfig.outputs[i]` (`kMaxSyncOutputs` = 16): for an enabled port the
kinds, and `delay` = `midiOutputLatency()[i]` + offset in frames; a
disabled port sends nothing. While the sync
source is MIDI clock or MTC on an input whose device is that output (names
equal without capture/playback, in/out, input/output, midiin/midiout), the
kinds the master sends are cleared (clock, transport, SPP, clock while
stopped; or MTC), with a warning in the sync warnings and on the port;
`sameRuntime()` compares the kinds, so a kind change builds a runtime and a
delay change adjusts in place.

**The callback.** A runtime with any sending output keeps the span ring
(as for LTC output: the timeline processed per cycle, the stopped position
in stopped spans, two spans at a wrap, 4096) and per output a
`ClockGenerator`, an `MtcGenerator` (the project's timecode) and where the
last piece ended. Each cycle, after LTC, for each sending output i with
total delay D = plan latency + hardware MIDI lead + delay_i (never
negative once the plan's lead covers the port; see below), the window `[c, c + n)` reads the
timeline at `[c − D, c + n − D)` as pieces: spans clipped to it (before
the oldest: unknown, nothing sent), then, past the newest span (D < 0), a
prediction at its speed wrapping at the loop's end. Each piece at output
frame a: stopped: Stop if running, MTC stops, and when it is a stop or a
seek (more than 5 ms from where the last piece ended) Song Position and a
full frame where it stands; with `clock_while_stopped` F8 every
60 / (24 bpm) seconds at the tempo there, continuing the last clock's
phase. Playing: start (Start at or before beat 0, else Song Position of
the next 16th and Continue) when not running, locate (Stop, Song Position,
Continue; MTC full frame) on a step over 5 ms, then clocks and quarter
frames inside the piece, beats through the plan's time map with its exact
inverse. Events go through a 2048-entry buffer, filtered by kind (F8
clock; F2 spp; FA, FB, FC transport; F1 and the full frame mtc), to
`writeMidi` at `ceil(frame) − c`, while the period's buffer is short of
the release reserve (`kMaxMidiOutputEvents` − `kMidiReleaseReserve`, every
writer counted). A Stop may use the reserve. `Port::slaveRunning` is what
was last written to the slave (Start or Continue, or Stop), not the
generator's state. A Stop for a running slave that does not go out
(refused, or no room in the event buffer) sets `Port::stopOwed`, which
only a written Stop clears: the owed Stop goes at frame 0 of each later
period until the device takes it, and nothing else goes to the port
before it. If the transport resumed meanwhile, the generator is stopped
as the owed Stop goes and starts again in the same period, so the slave
hears Stop, then Song Position and Continue (or Start at beat 0), then
clock. A Start or Continue that does not go out (refused, over the
ordinary limit, which they may not exceed, or no room) leaves the
generator running for a slave that is not: the generator is stopped
without a message and begins again the next period, so the slave hears
Song Position and Continue from where the transport is then, until the
device takes them. Refusals and a full buffer count in
`Engine::syncOutputDropped(i)`, sent messages in `syncOutputSent(i)`
(reset at start). Count-in cycles do not play, so nothing but the
optional free clock goes out until the timeline moves. Offline renders
send nothing.

**Leaving.** `SyncRuntime::farewell()` writes Stop (FC) at frame 0 on each
port whose transport is running or owed a Stop, stops the generators and
ends the runtime's MIDI output. The callback calls it on the old runtime as it
adopts a new one, and on request: `Engine::stop()`, when the callback last
published `syncTransportRunning_`, sets `syncFarewell_` and waits for
`syncFarewells_` to count it (at most max(50 ms, 4 periods)), then the
plan's and device's latency (at most `detail::kMidiOutputCloseWaitSeconds`,
250 ms), before it sets `shutdownRequested_`. The next running cycle is
silent and calls `HardwareMidiRuntime::finish(closing)`, which releases the
routed notes still held: one shutdown path, the Stop before the note-offs
on each port. `Backend::stop()`'s final zero-frame callback calls both
again as the fallback for a device that ran no more cycles; every backend
delivers its MIDI (JACK in a real period, PortAudio to the native sender
as closing, the dummy backend into its capture). The sender's `stop()`
keeps `closingMessage()`s from its queues (note-offs, CC64/CC123 at zero
and FC), stamped `max(now, lastDue[port])` on CoreMIDI and now on WinMM,
drops the rest, and on CoreMIDI waits for the latest stamp (at most
250 ms) before the port is disposed.

**Status.** `midi.outputs.get` returns `{ports}`, and
`engine.status.midi_outputs` the same list (`EngineApp::midiPortsState`):
routing's port list (see "Hardware MIDI routing (ADR-0072)") with, for each
output, `clock`, `transport`, `spp`, `mtc` and `clock_while_stopped` as
set, `sends` (what goes out now), `suppressed` (set but not sent to the
sync master's device), `delay_ms` (plan latency + latency + offset) and
`sent` (clock and MTC messages since the engine started). `dropped` sums
routed MIDI, clock and MTC and the backend's asynchronous loss. The
routing grid's hardware column `clock` flag (`route.matrix`
`hardware_midi`) is true when `sends` has `clock`.

### Master playrate (ADR-0045)

Project schema 2.1 stores master playrate and its `tr_0/playrate` envelope.
Playback and mix/stem renders apply the scalar or active rate envelope. File
source conversion, MIDI, automation, effective plugin BPM, metronome and transport
use the shared fractional clock; live input stays in device time. GUI controls
are independent clients of the command API and the cached state accessor below.

- `transport.playrate.get {}` is a query available without a running engine.
  Result: `{requested_rate: number, effective_rate: number,
  preserve_pitch: bool, envelope?: en_id}`. `requested_rate` is the saved
  scalar; `envelope` is present only for an active master rate envelope with
  effective points (including automation items). `effective_rate` reports
  consumed output, accounting for graph and backend output latency, rather than the
  requested or render-lookahead value. Without an engine it is 1.0. While
  the transport is stopped nothing is heard, and a steady scalar reads as
  the rate playing starts at; an envelope or a sync chase reads as last
  heard (the builder's song journey 2, F8).
- `transport.playrate.set {rate?: number, preserve_pitch?: bool}` is an edit
  returning the same result. Rate is finite 0.25..4.0, and omitted fields retain
  their values. Empty arguments or unchanged settings make no version unless
  the call writes an automation point. While playing, a scalar edit glides
  (the knob ramp and its low-pass); while stopped (the transport asked to
  stop) it keeps the plan and makes a seek transport.stop prepared stale,
  so play and a count-in prepare the seek again and start at the new rate
  with no glide (D2: a glide queued on the prepared seek played out after
  the play start). GUI drags use `Origin.gesture` for the
  same live model updates and coalesced undo as strip faders. The range of a
  future knob is display state, never a model constraint.
- `transport.playrate.apply-to-tempo {}` is an edit returning `{}`. It
  multiplies every tempo-point BPM by the scalar and resets rate to 1 in one
  version, retaining tempo IDs, beats, ramps, pitch mode and meter map. The
  transport must be stopped, with no recording and no active nonempty rate
  envelope. Scaled BPM outside 1..1000 produces `tempo.range` at the tempo
  point path before mutation. Item timebases still govern placement: this is
  not a sound-preserving bake for time-locked audio. Applying unity is a no-op.

`env.add {track: "tr_0", param: "playrate", points?: [...]}` creates the
unique global envelope using the existing point/timebase and automation-item
contracts; other strip/route/take targets are rejected. Values are finite
numeric ratios 0.25..4.0, not booleans or unit text. The setter captures points
through the existing strip writer in touch/latch/write modes; latch/write
hold through pause/stop. Like current strip faders this is coarse capture,
without explicit held-control override or touch-return behavior. Authored
points replace the scalar without an extra smoothing ramp.

Recording audio or MIDI requires saved rate 1.0 and an active effective rate
curve that is absent/empty or everywhere 1.0. `record.playrate` reports the
scalar or envelope path and a hint to restore unity/disable the envelope.
Check before files are created, at engine recording/load boundaries, and
before committing model edits during recording; batch failure rolls back both
model points and transient writer holds. Commit validation failures restore
writer state too, including for internal transaction callers.
Undo/redo restore history on refusal before publishing any engine/event
change. Branch switching rejects an unsafe destination; merges into the
recording branch reject unsafe source/replayed snapshots as invalid merge
conflicts, including fast-forwards. Project replacement follows its existing
stop/commit-recording lifecycle before replacing the session.

The model helpers `activePlayrateEnvelope` and `validateRecordingPlayrate`
are model-thread APIs; no effective-point expansion runs on the callback.
Recording checks the audible and queued trajectories too: requesting unity
while an older non-unity transition is still queued does not make recording
safe. Removed curves remain guarded until graph and backend output latency has
drained their old trajectories, even if the current audible rate happens to be
1 before a queued non-unity span. A pitch-mode-only transition at unity is safe. Stop and restore unity before recording in that case. Portable
command/guard and executor tests run on every supported platform.

### Master playrate DSP building blocks (ADR-0045)

The internal `playrate_dsp` module supplies the fractional `PlayrateClock`,
original tape reader and streaming `StretchReader`. Plans apply them to each
file source before take FX and before live input joins. Instruments retain
note pitch; effects, live audio/MIDI, Console 9 and frontier rings run in device
time. A fresh exactly-unity project without an active nonempty rate envelope
keeps the previous path and its bytes.

The clock advances once per device frame and exposes timeline boundaries and
inverse boundary crossings. Manual scalar changes can ramp over a caller-supplied
number of device frames; authored envelope values replace the scalar immediately.
Timeline positions retain their fractional part across arbitrary caller blocks.

Tape conversion uses a shared, control-thread-prepared table: 256-tap Blackman
windowed sinc, 1024 fractional phases, and 13 anti-alias cutoff banks covering
1..4x in quarter-rate steps. Banks round up in rate (conservative cutoff), with
a 6% transition guard. Ratios below unity use the 1x cutoff. Integer source
positions at exactly unity read the original samples directly. Both channels
share phase and cutoff; file padding/looping belongs to the source reader.

Pitch-preserving conversion configures Signalsmith Stretch for all source channels before
publication and processes only 64-frame output quanta. The channel count is unrestricted and phase is linked across all channels.
Decoded in-memory data is fetched once per channel/64-frame quantum, returning
a direct contiguous span where possible and using prepared scratch for padding
or wrapping; neither tape taps nor stretch frames make per-sample indirect
reader calls. Source consumption keeps
a fractional cursor, using successive integer boundaries for input counts.
Source lookahead compensates both its analysis latency (source frames) and
synthesis latency (device frames). For a quantum ending at device frame D,
the required source endpoint is timeline(D + synthesis latency) + analysis
latency. Priming supplies the corresponding analysis window and future source
frames, with no graph latency. The owner
can spread priming over a bounded number of quanta per cycle, waits for ready,
and owns the audible transition. No reader allocates, frees or accesses files
while seeking, priming or processing. Unprimed/discontinuous trajectories are
refused without changing the stretch state.

A documented MIT vendor patch adds `reset(seed)` and resets silence/seek/smoothing
state. Its default RNG uses a fixed SplitMix64 integer sequence and explicit
24-bit float/53-bit double uniform conversion, independent of standard-library
engine/distribution choices. Plain `reset()` retains the current RNG state;
explicitly supplied custom engines keep upstream distribution behavior. Existing
whole-file media variants keep their seed and processing schedule, with the new
portable phase sequence. Steady-state quality checks measure RMS and local tone
energy rather than a phase-sensitive long-window amplitude. Shared portable DSP
tests exercise partition-independent clock/ramp output, exact unity,
linked stereo, tape anti-aliasing, preserved pitch, unity impulse alignment,
repeatable seeks at extreme
rates, non-48k priming and invalid trajectory rejection, including under RTSan.

`EngineApp::playrateState()` is a model/UI-thread accessor for GUI synchronization:
`{requestedRate, effectiveRate, preservePitch, envelope?, envelopeValue?}`. It
allocates no memory, executes/logs no command and creates no history entries.
Effective rate comes from `Engine::effectivePlayrate()` consumption telemetry. The optional envelope value evaluates the
latest model curve at the consumed transport position; it is distinct from the
possibly older audible effective rate. Effective points are expanded/cached when
the model head changes, including history and branch changes, not on each GUI
sync. The accessor works without a running engine, using the stopped position.

The plan owns two bounded windows of a single fractional timeline trajectory:
the renderer advances one, the callback advances the other, and their node jobs
only read prepared windows. Device/stream frames remain monotonic through loops;
timeline position wraps. Accumulation uses absolute project samples, including
for nonzero offline range starts, so a fresh seek and a render of the same range
make identical source phase decisions. PDC shifts **device** coordinates before
the clock lookup, using the backwards-solved `timelineShift` rather than forward
arrival. Source-ahead/behind and clock margins use that same shift, including
take latency and stage offsets (ADR-0051).
All source converters share a 64-frame grid in compensated source time, including
preroll: two identical sources with different take-chain latencies remain aligned.
Looping retains converter history and reseeding occurs only for a fresh seek epoch.
The reader is silent outside an item's span (trimmed media is not on the
timeline), so past a loop seam the retained history holds what the loop end
played there, and an item starting just after the loop start sounds the same
every pass. A trimmed item's content is therefore independent of its
trimmed media. (Reading that media would change tape conversion only within
its kernel's reach of the item's edges, but the pitch-preserving stretch,
whose phase depends on what it was fed before, over the whole item.)
MIDI uses the first device boundary reaching each timeline event; compensated
instruments wrap/release timeline notes in their own input coordinates, preserving
live notes. Strip/send/take automation uses the same mapping. Plugins receive
timeline seconds/beats and effective BPM (project BPM multiplied by rate); BPM is
sampled on the canonical 64-frame control grid. Other source and event timing
retains per-frame precision. Clicks are synthesized in device time at mapped beat
crossings, retaining their pitch and 40 ms duration. Count-in freezes the timeline
and uses the selected rate at its start.

Compatible scalar-only live edits (ADR-0046) retain the current graph and source
converters. Persistent collection identity detects that only the project scalar
changed, without traversing tracks. Unchanged-session and plugin maintenance
reloads retain graph compilation. A rate-enabled plan glides through exactly
1.0 like any value (a MIDI knob's centre lands there), on the converters it was
compiled with; once rate scheduling has started the engine keeps rate-enabled
plans until it stops, and tape reads at exactly 1.0 are the original samples.
Each structural edit then prepares a rate plan; level and pan edits do not
(ADR-0060, §1.6).
While a prepared rate plan covers audio not yet played, `load()` defers a
structural edit: it returns success at once, and the edit compiles once that
span has played (in `service()`, or in whichever `load()` comes first). That
load always compiles: neither the scalar nor the mix shortcut takes it, even
when a playrate move is still pending from a full queue (the compile carries
the pending value). If that compile fails, the edit never played. The engine then keeps the session
it plays as its latest, for later reloads, the mix and scalar shortcuts and
the instance prune, with a playrate move that a full queue refused before
the deferral still pending (it is published once the queue has room), and
adds a warning. `takeUnplayedEdit()` returns the error
once; `EngineApp` emits it as `engine.error` and reports the version that
plays as `prepared_version`. The project's head keeps the edit: undoing or
changing it reloads. Whatever the engine names as latest, the instance pool
is pruned against the session the newest published plan was compiled from,
so no instance a running plan uses is destroyed.
Two cases prepare a plan instead: leaving the unity bypass (no rate plan yet),
and leaving 1.0 on a preserve-pitch plan compiled at 1.0, whose sources read as
tape and need the stretch converter. When that prepared transition only changes
the scalar from a steady 1.0 in the same mode, its clock holds 1.0 through the
converter crossfade and then glides like a knob move (the longest ramp and the
same low-pass), not in 10 ms. The renderer accepts a prepared plan whose
predecessor is any unity-bypass plan when it runs one: the consumed clock can
name an older unity plan than the renderer's, and unity plans share the
identity trajectory. Two bounded SPSC queues carry identical
64-frame device boundaries and values to the executor timelines; a shared
release/acquire gate commits both queues together. Boundaries follow both
already generated clock windows plus a render/device block of publication
headroom (at least 128 frames), so existing PDC and
stretch lookahead is respected. With the anticipative renderer they also lie
past every window it may generate before it takes the edit (the render-ahead
span beyond the device position last played, one render block and the windows'
lookahead): a renderer catching up to its target could otherwise pass a
boundary taken from its windows, and the late edit would force a recovery seek.
When the renderer is caught up, that is where its windows already end. Each timeline ramps linearly from its current
scalar, without restarting the converter or replacing the plan/epoch, over one
and a half times the audio played since the previous scalar edit (at least one
device block and 10 ms, at most 300 ms; 300 ms after a pause or a new epoch):
the next edit normally arrives mid-ramp and continues from there. The scalar
follows that chain of ramps through a one-pole low-pass with a 150 ms time
constant (`PlayrateClock` smoothing, part of its state, so checkpoints and
structural replay reproduce it), which removes the corners where ramps meet:
a mouse drag's or MIDI knob's steps become one glide with continuous rate and
slope. It settles exactly once within 1e-5 of the target. Smoothness takes
priority over response here. Authored
rate envelopes remain authoritative and retain their shapes. A control-side
history replays scheduled future scalar edits during structural preparation.
Queue saturation coalesces the latest value for service to retry. Ordinary
60–100 Hz edits can arrive during an earlier ramp and do not debounce until the
gesture ends. The saved scalar remains the offline render setting; arbitrary
live gesture timing carries no offline null promise.

Unity bypass crossings and changes of converter/rate configuration prime
the selected source state and crossfade for 10 ms. The old path runs only through
that transition. All source allocation, configuration and priming run on the
control thread before publication, including non-seek anticipative edits. An
ordered control-to-renderer queue carries prepared plans to a future canonical
stream boundary measured against the renderer's actual frontier (consumed
stream in callback mode), rounded to 64 frames with headroom for block
granularity and observed preparation time. Lateness validation uses the same
frontier and retries farther ahead; the renderer validates that boundary,
transfers source state and publishes the callback handoff through RT-annotated functions. It
caps old-plan rendering/virtual advancement at that boundary. A late boundary or
predecessor mismatch requests a prepared seek on the control thread without
allocating or priming on the renderer. A prepared-plan queue preserves
the exact sequence of trajectories already rendered. Control-side transport,
play/count-in and recording queries use an immutable prepared-boundary snapshot,
not the renderer-to-callback handoff fields. Converter configuration, saved anchors,
origins and transition metadata are finalized before publication; scalar controls
use the queues above. Control-side prediction reads immutable curves, the
control-owned scalar log and atomic tagged boundaries/floors.
Source state and compensated
delays carry across unrelated model changes. The usual edit response includes the
render horizon (normally 200 ms). For alignment, a prepared bypass/mode/curve edit
additionally waits for maximum compensated source lookahead and a canonical
boundary. Further requests requiring preparation coalesce while the source
transition finishes; requested model settings and undo still update immediately. This can delay changes substantially on long
latency take chains. Slow source preparation can extend the schedule; failure
publishes no partial trajectory. `Engine::prepareSeek(seconds)` prepares fresh
source state off the callback before publishing the seek epoch, returning a structured
error if preparation fails. The old playback and live monitoring continue until
publication. Stopped playback also prepares before resuming.

Offline ranges are timeline ranges; inverse clock boundaries determine output
length. Tail duration stays in device seconds, and fixed output PDC is cropped
in device frames. `OfflineSettings::applyMasterPlayrate` defaults true; analysis
taps and item-FX renders set it false. Converted output is capped at 2^28 frames;
clock history has a separate bounded capacity and reports `playrate.budget` when
plugin latency exceeds it. Every rate plan reserves four additional seconds of
past/future clock headroom beyond its current PDC and converter margin, including
zero-latency graphs, so ordinary multi-second latency insertions retain the
existing trajectory. Ring capacities round up to powers of two. Unrelated edits
copy the available tagged past, retaining earlier rate curves rather than
reconstructing that history from the current rate. If a running edit
needs history outside that window, or preparation repeatedly misses its future
boundary, the engine automatically prepares a fresh seek at the consumed position
using the latest requested model. Exceptionally late scalar control publication
uses the same recovery. It publishes the replacement plan before its
seek epoch, through the same path as a locate. The anticipative renderer requests
that preparation from the control thread and continues the old plan while it
waits, without retrying the failed adoption. Live monitoring continues, and
playback stays requested. Like a locate, this exceptional recovery resets source
and FX state and may have an audible discontinuity/preroll; it requires no user
pause/reload and emits no `playrate.history` or `playrate.prepare_late` error.
Volume moves and compensated bypass normally inherit the trajectory without a
seek; a busy machine can still invoke the same late-preparation recovery. A PDC
edit that moves an existing pitch-preserving source in compensated device time
also uses a prepared seek, because clock samples alone cannot reconstruct its
past converter state with the phase of unchanged sources.
Source trajectory violations fail the render/process rather than silently
presenting converter silence as successful audio.

Render-command preflight accounts for scalar speed and conservatively uses the
minimum effective envelope-point value when a curve is active. It includes the
unscaled device-time tail and the larger of engine/export sample rates; requests
whose upper bound exceeds the in-memory frame budget fail with `render.budget`
before source preparation, files or jobs. Public `renderOffline` also checks
finite timeline endpoints and rejects obvious expanded-duration excess before
media preparation, then checks exact inverse-clock output length. Analysis taps
and item-FX renders opt out of this master-rate expansion as well as conversion.

## VCA gain controls (ADR-0052)

Groups compile to control data, never audio nodes or latency constraints.
At each member fader, sum its evaluated dB level and every unique controlling
VCA's evaluated level in numeric ID order. Silence on any contributor is
absorbing. Clamp the final effective gain to the existing Console 9 ceiling
(gain 4, about +12.0412 dB), including VCA-controlled masters. The requested
sum and applied level are queryable. Ordinary unassigned master behavior is
unchanged. Mute is the OR of member and controller mute; VCA solo adds its
transitive track members to the ordinary solo seed set, retaining solo-defeat
and signal-path reachability. One function decides solo silencing,
`model::soloSilencedTracks`: plans and `model::effectiveTrackLevel` both call
it, so the audio and the readouts cannot disagree (ADR-0093). Member
fader/envelope data never moves.

The combined control is applied before encoding, at the member's fader.
Post-fader send encoders first clamp the combined source/VCA level at the
member's native ceiling, then add the route contribution before their own
encode. Send attenuation therefore cannot recover above-ceiling source gain;
a shared post-fader tap already contains the clamped member level. Pre-FX
and pre-fader sends stay independent of the member fader/mute. Post-fader
key taps include VCA volume/mute without audible-path solo suppression.
Main-to-aux post-fader links apply the source's combined gain before their
edge delay; auxiliary source channels themselves remain unfaded.

Each node/send consumer owns its curve cursors. Curves shift by the existing
ADR-0051 application coordinate and use the appropriate executor's fractional
playrate clock; locates, seeks and loop wraps reset them. Compilation rejects
more than one million copied VCA curve points before publication. No VCA graph
traversal, allocation or locks occur on audio threads. Existing ramps and
Console state carry across plan swaps. Offline, callback and anticipative
execution share the same sample processing, including through bridged FX.
Member stems retain their VCAs and existing selected-tap trimming/decoding.

Through-master stems clear existing track and VCA solos before selecting the
stem target. VCA levels and mutes still apply; an unrelated soloed group cannot
leak into an exclusive stem. Isolated stems retain their member VCA controls.

## Track freeze (ADR-0054)

`track.freeze` captures a channel's own sound from timeline zero through its last
item, including take processing, all inserts, polarity/mono and every auxiliary
channel. It uses a private instance pool and the **full graph** schedule, tapping
pre-fader before the Console 9 encoder. It leaves the original objects and saved
plugin patches/blobs in the model. `track.unfreeze` checks that the saved graph
can prepare before restoring it. Missing plugins or render warnings refuse the
operation instead of publishing silence or placeholders.

`OfflineSettings.freezeTrack` selects that full-graph, multichannel capture;
`freezeProbe` prepares only and returns target chain latency and timeline shift.
`freezeTail` enables automatic tail capture, with `freezeTailLimitSeconds` an
internal adaptive allocation bound (2 seconds initially, doubling to 600).
The result carries `freezeLatency`, `freezeShift`, and `freezeTailFrames`.
Serial insert tails add; parallel take chains contribute their maximum. Reported
tails are sampled during processing. Capture ends only after the reported tail
and one continuous second below -120 dBFS on every captured channel. An infinite,
nonfinite, excessive or unsettled tail refuses publication. A bridge reporting a
longer tail only after service refuses with `freeze.tail_changed`; in-process
isolation can capture that dynamic tail. Plugins reporting no tail use the same
silence rule; an unreported delay longer than that quiet window cannot be inferred.
Capture is bounded to 2^28 total samples across channels and ten minutes of tail.

The frozen plan omits all original channel/take/input plugin stages and item
sources. One localized MediaCache preparation loads/resamples the cache outside
both executors. The shared RT reader copies cached samples at
`position + origin - timelineShift - virtualChainLatency`; outside the captured
interval and while stopped it emits zero. It supplies the pre-fader and auxiliary
buffers, then the ordinary fader/pan/mute, Console 9, sends and parent routing run.
The captured latency remains in the normal solve, including live automation
alignment. No cache work, allocation, locks or files occur in the callback.
Frozen FX ignore MIDI parameter moves and queued editor output.
The racks on a frozen channel keep running (their Out modules may also feed
rack sends, ADR-0075), so their plug-ins are not frozen FX and are not pruned;
the freeze locks them instead (rack.md §6).
Original live instances are pruned only after both executor epochs release the
old plans. Dedicated bridges exit; a shared bridge removes the frozen instances and
keeps its process for unrelated instances and later reuse. A frozen FX description reads stored state without creating a plugin.

Publication is one private typed `track.freeze.commit` edit, not a head-version
compare. It checks project generation, branch and media directory, recomputes the
sound-dependency hash at the current head, verifies input file hashes, and probes
the full head graph for unchanged target scheduling quantities. Unrelated edits
can therefore complete while rendering. The replay record contains the captured
states and verified cache metadata; redo/merge reuse that file. Unpublished job
artifacts live in the reserved `.freeze-pending` media subdirectory, excluded
from Save As copies. Publication renames into media before committing the
reference; cancellation/failure removes the pending artifact. Published files remain as ordinary
media for undo, branches, autosaves and Save As.

V1 accepts disarmed channel strips at unity master rate with no active master-rate
envelope or external audio/key/aux/MIDI input. A monitor setting on a disarmed
channel is inactive and allowed. Freeze locks tempo, meter, project sample rate,
master-rate and captured content (including pooled edits). Mixing and outgoing
routing stay live. This captures an offline performance, not transient plugin
DSP memory: random plugins, transport-sensitive state, new seeks and repeated
loops can differ from running the original plugins. Plugin binaries, plugin-owned external files
and private state absent from its exported patch/blob cannot be hashed by the
host; keep those assets stable during capture. Cached playback itself uses the
same reader offline and in both RT executors. At another device rate it resamples
the recording; equality to a fresh unfrozen render at that new rate is not promised.

### Prepared audio sources and take warping (ADR-0053)

`PreparedSources` is an immutable complete set of `PreparedSource` buffers keyed
by take ID. `MediaCache::prepareSources(session, directory, rate, cancelled)` performs
file decoding, resampling, scalar variants and piecewise warping without plug-in
instances. `adoptSources(shared_ptr<const PreparedSources>)` installs the set on the
model thread before compilation. A plan retains the set and its audio leases. Other
source-producing features can supply these same immutable buffers; the service has
no freeze-specific policy. Offline renders select required takes in the full pinned
session context, retaining every phase-bundle input even for a single-track render.
Unrelated tracks are excluded from source preparation, so their missing media cannot
prevent a selected stem from rendering.

`sourcePreparationKey` contains source identities, sample rate, active-take selection,
item durations and source-affecting take fields. Transient overlays, gain/pan, mute
and FX do not change it. Unhashed files also include size and modification time.
EngineApp source changes run on one background `SourcePreparation` worker with one
replaceable pending request and one completion. Requests cancel obsolete work at
DSP chunk boundaries. The worker owns a private cache; it never mutates the live
cache. Completion wakes the existing portable model-thread relay. Recheck the latest
source key before adopting/reloading. The prior ready plan remains audible while
preparation is pending; no partial bundle or silently unwarped substitute is published.
`engine.status` adds `preparing_audio`, `preparation_error`, `prepared_version`
(last compiled model version) and `preparation_version` (pending target, zero when idle).
The worker retains at most 2^26 sample values of reusable variants between requests;
eviction never invalidates outstanding plan leases. Startup and explicit
offline rendering wait for preparation. Shutdown joins the worker before releasing
its cache or wake target. Decode calls themselves are not interruptible mid-call.
Prepared source snapshots retain their immutable input session and target sample
rate as provenance for source-dependent edits; this adds no separate audio cache.

An active warp is a valid nonempty piecewise-linear map on nonlooped, pitch-preserving
Signalsmith audio. Invalid legacy maps and unsupported modes retain scalar playback
with a plan warning. New commands refuse these modes. The stretcher uses 64-output-
frame scheduling, seeded synthesis, explicit lookahead/history and zero padding.
It keeps synthesis state across short marker intervals and trims preparation latency.
An identity map with no pitch shift copies samples exactly. Prepared warped clips
cover `ceil(item_duration * sample_rate)` frames so separately rounded project
boundaries retain their final frame. They start at offset zero; take FX, fades, gain,
channel mode, Console 9 and the master playrate converter remain downstream, with
ADR-0051's original timeline shifts.

Reciprocal `warp_peers` use one aligned multichannel stretch instance with canonical
ID/channel ordering. The prepared outputs are separated back into their individual
takes before gain/FX; per-take rendering does not run separate phase estimators.
Exact duplicate and polarity-reversed input relationships are restored from the
canonical channel after synthesis, avoiding low-energy phase fallback differences.
At most 128 channels and 2^28 aggregate packed-input-plus-output sample values are
allowed per bundle. A complete preparation request also limits its estimated decoded
plus output samples to 2^28 before decoding. Shared decoded sources and scalar
variants count once, so repeated clips do not multiply their storage estimate.
Source/out-of-file regions are zero padded. No marker lookup,
analysis, stretching, allocation, file access, cache access or worker wait runs in
an audio executor.

### Audio hit and quantize commands

- `take.hits.detect {takes:[tk], sensitivity?:0..1}` starts a job. Multichannel
  positive log spectral flux, local thresholds, low-frequency weighting and
  sample-envelope refinement with an energy-rise gate generate stable original-source
  positions (the gate rejects periodic flux on sustained bass). Source
  content is checked before/after analysis. The posted completion checks project,
  branch, source identity and unchanged hit state, then records one atomic result
  edit for all takes.
  `job.get` becomes done only after that edit commits. Actions are not batchable;
  `take.hits.set` provides the batchable result edit. Cancellation/stale results
  commit nothing, and the log stores positions rather than rerunning the detector.
- `take.hits.set {take, positions:[seconds], source_hash?, algorithm?, sensitivity?}`
  replaces candidates; empty positions records analyzed silence. Positions are
  ordered and within the original source. The default algorithm is `manual`.
- `take.stretch.add {take, source_pos?, item_pos}`, `.move {take,index,item_pos}` and
  `.remove {take,index}` edit anchors. First add pins the item boundaries. Indices
  are zero-based in item-time order; move/remove operate on interior anchors.
  Linked peers receive the same source-relative edit, atomically.
  Omitting `source_pos` inserts an identity point on the current source map at
  item-local `item_pos` seconds, including the take's offset/rate and existing warp;
  audio timing stays unchanged until the marker moves. An identical existing point
  succeeds without duplicating it. A conflicting point still fails map validation.
  Warp peers receive the point once, with existing lock/freeze/mode checks;
  ordinary item/track edit groups do not propagate take-content edits.
- `audio.quantize {items:[it],grid,strength?,swing?,mode?,reference?,hits?,crossfade?}`
  consumes nonstale stored hits. Defaults: strength 1, swing 0, mode `warp`, first
  selected item as reference, hits `reference`, crossfade .005 seconds. `hits:merged`
  unions candidates in project time, coalescing within 5 ms. Grid is quarter-note
  beats in 1/1024..1024; strength/swing are 0..1. Odd grid points are delayed by
  `swing*grid/2`; nearest ties go earlier. Strength blends in project beats, then
  the tempo map converts back to seconds. Crossed/colliding anchors or boundary
  targets refuse the whole edit with a finer-grid/lower-strength hint.
- Warp quantize authors one shared map for aligned item spans, rates and pitches;
  all existing linked peers must be selected. Slice mode requires unwarped takes,
  splits all selected items at common times, then moves them identically. Strength
  zero splits without quantization. Short linear fades use pre-attack material;
  gaps fade to silence rather than synthesizing or repeating attacks.
- `audio.warp.reset {items:[it]}` removes active-take maps while preserving hits,
  scalar rate and pitch. `audio.warp.detach {items:[it]}` explicitly detaches selected
  active takes from phase bundles while retaining maps. Membership changes update
  all affected peers and respect locks.

`take.list` exposes `stretch_markers`, `transients`, `warp_peers`, `hits_stale`;
`session.describe` includes hit/anchor counts. All edit commands are one history
step, with structured errors and no source I/O in the quantization handler.

`take.set` accepts `rebase_seconds` for an item-left trim: it evaluates the source
map at the new origin and translates all anchor item times. It is mutually
exclusive with `start_offset`, which slips the map in source time. Changing
`playrate` scales anchor item times. `item.split` applies the same rebase to the
right take. Clipboard copies remap a complete warp bundle to fresh take IDs;
partial copies are independent. Edits that break a remaining bundle's alignment
fail at transaction commit, allowing a batch to move all members together.

### Mixing items to a new track

`item.mix-to-track {items:[it...]}` is a background action returning `{job}`;
`job.get` becomes done after one internal `item.mix-to-track.commit` edit (one
undo step) and its result holds `track`, `item` and `file`. It sums material
mid-session like a buss, rendering FX and joining files into one.

Only the selected items sound. Each plays through its channel's full strip:
take FX, item and take gain, fades and pan, the track's FX chain, its
automation (mute automation included), polarity/mono, fader and pan and the
Console 9 Channel encode. The encoded outputs are summed and decoded by a
neutral Console 9 Buss: the render reads the pre-FX tap of a fresh buss under
the main master in a private snapshot whose only children are the source
channels (parent sends enabled), so the file is exactly what a new buss fed by
those channels hears at its input. With `project.console9` off the stages run
linear as everywhere else. Sends and the channels' own parent busses are not in
the path; every track's mute switch and solo, and VCA mute and solo, are
cleared in the snapshot, so the selection alone decides what is heard. MIDI
items play through the track's (or take's) instrument. Unselected items are
left out of the snapshot view, so plug-in key inputs fed by other tracks hear
silence. The master rate is not applied (the result plays under it).

The span runs from the earliest selected item's start to the latest one's end,
with no tail, as `item.consolidate` and `item.apply-fx` do; effects ringing past
the last item's end are cut. The result is a project-rate stereo 32-bit float
WAV in the project's media folder (pending under `.mix-pending`, excluded from
Save As copies, removed on failure or cancellation). The commit adds a new
channel, muted, named "<source> mix" for one source channel and "Mix" for
several; it goes after the last source track in arrange order, under their
common parent, else under the main master after the top-level strip holding
the last source (at the end of the main master's strips when that is under
another master). One item covers the span from its start; the source items
stay untouched. Selected items not on a channel refuse with
`item.mix_not_channel`, items on a frozen channel with `track.frozen`, a
selection with no unmuted material with `item.mix_empty`. Changed selected
material (`itemEditFingerprint`) or another project or branch at completion
refuse with `item.mix_stale` / the render target error; mixer changes made while
the job runs do not. Render warnings (missing media or plug-ins) and nonfinite
output fail the job with `item.mix_render`.

### Consolidating items

`item.consolidate {items:[it...]}` is a background action returning `{job}`.
`job.get` becomes done only after one atomic edit publishes all results; its
result contains `items` and audio `files`. Partition the explicit selection by
track and active source kind: one audio and one MIDI result at most per track.
Each spans its group's first start to last end. Audio uses only active takes;
inactive alternatives remain available through undo. A MIDI group of several
items becomes one item from the first start to the last end with what each
item's heard take plays (looped passes unrolled, notes ending by their item's
end; gaps empty, overlaps layered), keeping the earliest item, its id,
settings, take name and take FX. A lone MIDI item whose takes are all MIDI
(two or more) has them layered into one take; any other lone MIDI item stays
unchanged. A selection with only such unchanged groups has nothing to
consolidate (`item.consolidate_empty`).

Audio capture bakes item mute/gain/fades, take gain/pan/polarity/channel mode,
take envelopes, source looping/rate/pitch/warp and take FX with their automation.
Track/input FX, strip controls, routing, VCA and master-rate conversion stay live.
The existing renderer captures PreFader from a derivative snapshot with the target
track's processing/input routes neutralized; PreFx is before item sources and
is not a suitable capture tap. Take output is cropped to its item bounds as in
playback, leaving silence in gaps and layering overlaps. No extra tails are added.
The result is project-rate stereo float WAV, placed with project sample-boundary
rounding, in the project's media folder. Keep the earliest item ID, placement,
lane/timebase/group and presentation, replacing its takes with one neutral take:
zero source offset/pitch/gain/pan, unity rate, normal channels, no envelopes/FX,
looping, mute or fades. Track processing then applies once.

`model::consolidationGroups` is the shared read-only command/menu planner.
Different lanes or item-group tokens within a partition refuse with
`item.consolidate_layout`. `completeItemSelection` reuses audio-warp validation
and requires all affected warp owners and linked edit peers, respecting locks,
freeze and group suspension; incomplete selections report a repair hint.
There is no additional item-count cap: existing source and render sample budgets
apply to each capture. `itemEditFingerprint` guards selected material and owned
automation/timing without including unrelated mixer controls; IDs normalize for
branch replay. Input hashes include all preparation peers (even inactive takes
whose channels influence bundle phase), are checked on the worker, and file stamps
again before publication. Project/branch/destination changes or changed selected
material refuse the completion with `item.consolidate_stale`.

Owned pending WAVs live under `.consolidate-pending`, excluded from Save As copies.
Cancellation or failure publishes no model change and removes owned artifacts.
One internal `item.consolidate.commit` edit verifies saved media, attaches all
audio results and merges MIDI atomically. Replay reuses those files rather than
rerendering FX. Published files remain available to history, undo and project
copies. Errors carry paths and repair hints; source/render problems must be fixed
before retrying. No sample processing or I/O is added to audio executors.

### Exporting an item

`item.export {item, path}` is a read-only action: it writes a file and changes
nothing in the session. It exports the active take over the part the item covers,
so trims and splits are respected: a split piece exports only its own part.

Audio is written raw: the source's own samples, sample rate and channel count,
with no item or take gain, fades, pan, polarity, channel mode, take or track FX.
The span is in source time: from the source position at the item's start to the
one at its end. Without an active warp map that is `start_offset` to
`start_offset + length * speed`, where `speed` is the playrate times the pitch
ratio when pitch is not preserved (the playback variant's speed); with one it is
`audioSourceAt` at the item's two ends. Rate, pitch and warp decide the span but
are not applied, so the file lasts the source time the item reads, not the item's
length, when the rate is not 1. A looping item ignores markers, as playback does,
and wraps around the source as heard (a loop longer than its source repeats it);
a non-looping item's span outside the source is silence. Ends round to the
source's sample grid (`samplesAt`), so a split's pieces tile the source without a
gap or an overlap. A 16- or 24-bit PCM WAV source keeps its format (written
without dither, so it is bit exact); every other source is written as 32-bit
float WAV. Exports over the renderer's 2^28-sample budget refuse with
`item.export_too_long`.

MIDI is what the item plays (consolidation's heard-take helper: the source window,
looped passes unrolled, a note held into the start begins at 0, notes ending by
the item's end), in item beats from 0 at the source PPQ, as a format-0 Standard
MIDI File with the tempo and meter in effect at the item's start (one of each).

Relative paths resolve against the work directory like `project.save`; a path
without an extension gets `.wav` or `.mid`; a mismatched one is
`item.export_extension`. Missing folders are created and an existing file is
replaced, except the project's own media (`item.export_path`). An empty take is
`item.export_empty`; an unreadable source is `item.export_source` with a relink
hint. The result gives `path` and `format` (`wav` or `mid`), plus
`sample_format`, `sample_rate`, `channels` and `frames` for audio or `notes` for
MIDI. The source is decoded on the command thread (as analysis commands do); no
audio executor is involved.

### Healing audio splits

`item.heal {items:[it...]}` is a pure edit returning surviving `items`, one per
track, in one undo step. `model::healingGroups` is the shared read-only eligibility
and reconstruction helper. Each selected track must have at least two consecutive
audio pieces on one lane, with complete linked/warp selection. No source I/O,
sample search, live-plugin queries or rendering happens in the handler.

Touching pieces must meet at the same project sample boundary, without interior
fades. The one overlap exception recognizes unchanged Slice-at-hits pre-roll:
at most 50 ms, bounded by half the preceding logical slice, matching linear
zero-curve interior fades, and the same global source map through the overlap.
Gaps, moved pieces, nesting, unrelated overlaps and altered interior fades refuse.
Healing restores continuous pre-split/pre-slice audio; removing the generated
internal fades intentionally differs from the currently faded slice mix.

Items must match lane, stored/effective timebase, loop, mute, gain, group token,
take count/order and active index. Every corresponding take must be audio from
the same media ID, with matching stored properties, including rate/pitch/mode,
gain/pan/polarity/channels and original-source marker/transient metadata. Source
offsets and complete stretch maps must represent the same rebased source map on
the project sample grid. Keep the first item/take IDs and full authored maps,
first fade-in and last fade-out (complete shapes/curves), and first presentation/
snap point. Extend only the full span and remove redundant right-hand objects.

Take FX chains must match their complete stored plugin/settings/patch/parameter/
state-blob values and normalized owned automation; instance activity is irrelevant.
No special normalization of drifting stored FX state is performed. Corresponding
FX envelopes retain their full matching original curves/automation. Take-relative
envelopes may join constant or linear seam continuations, with copied automation
rebased to the original origin. An unrecoverable nonlinear seam refuses with
`item.heal_envelope`; differing stored FX refuse with `item.heal_fx`. Other
incompatibilities use `item.heal_incompatible`, all with a Consolidate hint.

All original left-side warp peers must be represented by compatible selected
chains. Retain their original take IDs and reciprocal ordering; right copies
must be detached as the split command leaves them. Validate restored spans/maps
with existing `validateAudioWarp` and group validation, without a second bundle
alignment implementation. Removing takes also removes owned envelopes' automation
items, so neither healing nor consolidation leaves orphaned automation blocks.

### Splitting at a prior zero crossing

`item.split {item, at, snap?: none|zero_crossing, bypass_groups?: false}` keeps
the existing `{left, right, linked_left?, linked_right?}` result. `at` is a project
beat strictly inside the item (`item.split_range` otherwise). Default `none`
preserves the ordinary audio/MIDI split. `zero_crossing` requires an active audio
take: MIDI and empty takes return `item.split_not_audio` with a hint to use
`snap: none`. The GUI skips those items before issuing calls.

The command searches the existing immutable prepared playback source, before
take FX, fades, gain/pan, track processing and master transport-rate conversion.
Scalar resampling/stretch/pitch, source offset, looping and item-local warp audio
use the playback clip's sample/offset rules. Channel mode is applied first; normal
stereo uses the channel with greater squared-sample energy in the search window,
left on ties. This avoids anti-phase cancellation and silent-channel bias; only
the first stereo pair is heard by current playback. MonoMix explicitly sums that
pair. ReverseStereo swaps the pair; temporal reversal is not a take feature.

Search at most 100 ms backward in item/project time, excluding item boundaries.
Choose the latest sample boundary at or before `at` with an exact zero or a sign
change from its predecessor; do not interpolate or search forward. Nonfinite
pairs are ignored. A fractional cursor in silence stays exact; without a crossing
(including DC), keep the requested cursor. A crossing in one channel does not
guarantee silence in the other or account for discontinuities caused by FX.

The source snapshot must match the active take's source settings and media
identity; warped data also requires matching item duration and phase peers.
`item.split_source_pending` asks callers to start the engine if necessary, wait
for preparation and retry. `item.split_source` asks them to repair/relink failed
media. No decoding, stretching, file access or new cache is introduced in this
edit handler. Multi-item batches may consume the same matching source snapshot.
Linked peers split at the initiating item's resolved beat, without independent
searches. Existing locks, freeze and warp-bundle validation remain in force.
History logs the resolved beat with `snap: none` so branch replay needs no audio
search or source preparation. Each successful command or batch is one undo step.

### Pan control MIDI learn (ADR-0066)

`track.learn.*` accepts `width`, `pan_left`, `pan_right` on channels and
busses with the pan shape (−100..100, centre detent, one percent per encoder
step), soft takeover allowed, and `pan_boost` as a switch like mute.
Masters refuse them with `track.strip_type`. Compensation edits preserve
the effective law's dB value and set its boost through `track.set {pan_law}`;
this records a writing envelope when the effective boost changes. Linked
manual edits carry positions relatively and mode/law absolutely; playback
of a member's envelopes never propagates through its group.

### MIDI sends (ADR-0067)

Every channel has prepared `midiPreFx` and `midiPostFx` event buffers,
independent of its instrument. Timeline events (including loops and master
playrate), monitored device events and incoming routes merge at exact frames
before feeding its instrument. MIDI routes can form A → B → C chains through
channels without instruments. Post-FX is the chain's MIDI bus after the
chain (below; ADR-0068). A frozen channel's
pre/post-FX MIDI is its timeline MIDI; no MIDI is recovered from its audio
print. Outgoing MIDI send changes do not invalidate the audio freeze hash;
incoming external MIDI remains unsupported for freeze.

Channel/Note Out → channel/Notes edges participate in graph order, mixed
cycle checks and read-ahead PDC. Source-track incoming routes also feed that
track's Notes readers, ordered before the rack without depending on the
track's audio result. All MIDI sources of live destinations run live; no
MIDI crosses frontier audio rings. Event queues and voice storage are
prepared on the model thread, bounded by `engine.midi_budget`. Route mute
releases delivered notes and unmute restarts keys still held. Post-fader
sends additionally follow source-strip mute; earlier taps do not. No audio
fader, pan or Console 9 stage alters MIDI.

Each channel's held notes take slots of a prepared pool sized by how many
can sound at once: the most item notes its timeline overlaps (plus a margin
of 16 for late releases), every live key when it has a MIDI input, and the
voices of each incoming send's source (16 per Note Out). Its sends carry
slot ids, a send's voices are its source's slots, and every queue and
budget multiplies these voice counts, never the number of notes in the
timeline. Event buffers are sized per block: a channel's timeline window,
a release per slot, live input, and for each incoming send the events its
source carries in any `block` consecutive frames (`midiWindow`) plus a
release and restart per source voice per mute transition. A PDC delay only
shifts a send's stream, so one such window covers a block whatever the
delay (the queue holds two, plus the delay at the source's per-frame rate):
a relay chain grows by each hop's own voices, never doubling per hop (a
six-hop chain of 32-note chords reserves 1767 events at its end). Adoption on the audio thread therefore walks the held voices:
item notes keep their slots by musical identity, live keys by key, routed
notes through the unchanged send that delivered them. A send whose route,
tap, channel filter or remap, PDC delay or ends changed starts afresh: what
it delivered ends at the next block's start (also downstream through relay
channels), then the notes still held start again, so an edit anywhere
upstream releases and retriggers instead of doubling a voice. An onset on an
input that is still held ends the held note first. Equal pitches from
separate sources stay independent: each holds its own slot. Notes or events
past a prepared capacity are dropped and counted with `voice_dropped`
(offline: a warning naming the track). The instrument retains its existing
4096-voice timeline/device budget; incoming routes add their voices. A send's
channel filter and remap apply to channel messages only; system messages
pass unchanged.

MIDI sends ignore solo: a solo-silenced source still plays its MIDI. In the
solo signal path a MIDI send between channels counts downstream only:
soloing its source keeps the destination it plays audible, while soloing
the destination does not un-silence the source's own sound (an audio send
does both ways, since the destination's signal contains the source's). At a shared frame releases normally precede onsets. If that
would put a voice's release before its own onset (late events or playrate
compression), the frame retains input chronology. Prepared sorting scratch
keeps both channel and Notes event ordering allocation-free. All-notes-off
releases stay at their controller, before a same-frame reattack.

### Hardware MIDI routing (ADR-0072)

Project schema 2.10 additionally accepts MIDI-only routes from channels/Note Out
to `to.midi_device`, addressed by `route.add/set {to_midi: name, midi: {}}`.
Taps, mute and MIDI channel filtering/remapping follow internal MIDI sends.
Hardware source nodes and their dependencies run live in both executors.
The callback uses prepared descriptors and fixed queues; offline renders
never write devices. Missing output names warn; duplicate ambiguous names fail.

Route delay is `plan.outputLatency - source.timelineShift - tappedLatency +
backend.midiOutputLatency[port] + offset_ms * sampleRate / 1000` (rounded to
frames). Negative minimum delay adds prepared audio lead to all MIDI delays and a
final audio delay; lead is limited to one second and reported by `midi_lead_frames`.
Ports that send clock, transport, SPP or MTC (ADR-0073) count with
`plan.outputLatency + backend.midiOutputLatency[port] + offset`, so a clock-only
port faster than its audio gets the lead too; the sync runtime's plan and heard
latencies include it, and clock follows a lead change at once (no slew).
Engine output latency, heard timing and recording account for it. History is
allocated on the owner thread only when publishing a plan with non-zero lead, and
runs only during non-zero lead or a tap crossfade. It continues across stop, locate,
loop wrap and lead-unchanged edits while lead is needed. The first negative-offset
route after an inactive history primes silence once. On return to zero lead,
owner-thread reclamation waits for the final crossfade acknowledgment and retirement
of every plan that could use it.

Boundaries drop only queued timeline-origin note-ons, retaining delayed live onsets,
releases, other messages and mute markers on reset-off ports; reset-on routes
invalidate their queues through the route generation. A lead change crossfades fixed
old/new integer-delay taps over 5 ms without clearing history or changing their
playback rate. Rapid edits finish the active fade, then fade to the latest target.
The initial plan primes its full lead normally.

No hardware delay may exceed ten seconds. Limits: 256 routes, 65,536 pending
messages, 16 ports, and the existing 1,024 messages per device period, shared by
routed MIDI, clock and MTC (`Backend::midiOutputUsed()` counts every writer):
stop-time releases, clock, MTC and every routed message but a release
(note-off, CC64 or CC123 at zero) stop at 1,024 − `kMidiReleaseReserve`
(512), and what that refuses counts as dropped, so the last 512 are left
for routed note-offs, a clock Stop and a stop's CC64/CC123 resets. Equal-frame
messages preserve route/event insertion order. Overflow is counted. Timeline origin
is captured with each stream event before a slot can be freed or reused, carried
through the FX bus and hardware queue, and retained for mute-retriggered notes.
Live/device and generated plug-in notes keep device time across boundaries on
reset-off ports. Route and source mute automation is evaluated on the tapped
stream's compensated clock, including its source shift and master playrate
trajectory. Channel post-FX and post-fader routes read the chain's final MIDI bus
(ADR-0068), including plug-in note output; pre-FX routes read the original stream.
The bus keeps the chain's input clock, so its tap latency cancels against delivery
compensation. New or re-timed post-FX routes restart the bus's held notes, including
plug-in voices, rather than the pre-FX pool. Rack Note Out events already include
their latency.

Port disable gates the next callback and releases notes before the render-ahead
horizon; enabling and timing changes adopt with their prepared plan.
Held notes release on removal, mute, disabling/re-timing, stop/locate when
reset is on, and engine shutdown. Releases include sustain-off; boundary and
shutdown resets add all-notes-off on used channels. These controllers share
hardware channel state. `reset_on_stop: false` retains timeline notes across
transport stops/locates; engine shutdown always releases. While stopped, this
preference suppresses only timeline-origin releases whose pre-block slot was
high and Timeline. Live onsets and releases pass even when a slot is newly
allocated or reused from a timeline note.
CoreMIDI shutdown
stamps closing releases at `max(now, lastDue[port])`, after accepted events,
then retains its port/client until the latest submitted timestamp, capped at
250 ms from sender stop (`kMidiOutputCloseWaitSeconds` and
`midiOutputCloseDeadline`, shared with a future final clock Stop). The wait is
on the control thread and covers the remaining scheduled plan/device delay.
Longer horizons can outlast that cap and require native validation. No
endpoint-wide flush is used. WinMM still drops future onsets and sends releases
immediately; JACK still writes its final period.

`midi.outputs.get` and `midi.inputs.get` return `{ports: [...]}` with name,
index (-1 absent), enabled, offset_ms, latency_ms, dropped and structured warnings;
outputs also have reset_on_stop. `.set {port, enabled?, offset_ms?, reset_on_stop?}`
(input omits reset) applies a machine preference. Port names resolve by a unique
case-insensitive match among present or recorded names, stored with the exact
spelling; unknown or ambiguous calls refuse with the available exact names. Offsets are finite -1000..1000
ms, positive later. Input advances clamp to this callback's start; positive
input offsets carry across callbacks. Existing record MIDI routes perform input
routing. Disabled inputs do not reach monitoring, recording or MIDI learn.
Status includes `midi_inputs`, `midi_outputs`, and `midi_lead_frames`. Each output reports `lead_ms`, the shared added latency
from the prepared note routes, also applied to live monitoring.
Per-port dropped counts route scheduling/rejection and attributable output
sender/device loss; native input losses without a retained port remain in the
aggregate `midi_dropped`. Port latency_ms is the signed backend latency before
user offset/lead; inputs currently report zero device latency.

Preferences add `midi_inputs` and `midi_outputs` maps to
`supple.preferences/1.6` (1.8 adds the outputs' clock members, ADR-0073),
keyed by exact enumerated port name. Records retain unknown members, so a
later version's survive edits. Defaults are enabled, zero offset, output
reset_on_stop true and every clock member false. The engine writer raises
an older schema label to the current one. A disabled input also stops the
sync chase from following it. GUI and headless applications use the
same preferences file; embedded hosts may provide their own persistence.

The engine and GUI share `engine::kPreferencesSchema` for the current machine
preferences schema, including a new engine-owned preferences file.

A newer version's file is never destroyed (round 134 G2, for every future
bump). `engine::preferencesSchemaNewer` says whether a label is newer than
this build's (a higher minor of 1, or a higher major; an absent or malformed
label is not). Before every write over the file, the engine's MIDI records
writer and the GUI's `Preferences::save` call `engine::keepNewerPreferences`:
a file whose schema is newer is first copied, as it is, to the first free
name of `<file>.<label, '/' as '-'>.bak`, `….2.bak`, `….3.bak`
(`preferences.json.supple.preferences-1.12.bak`, `newerPreferencesBackup`).
A backup is never overwritten, so going newer, older, newer, older keeps
each round the newer version left. No copy is made when a backup of that
label already holds the same bytes, or when the file holds what this writer
last wrote there: the engine's writer then keeps the newer label and members,
as before, so its own writes still read as newer, and it remembers its last
write (`EngineApp`'s, per file) to tell them from the newer version's. A failed
copy refuses the write (`preferences.backup`). The newer version reads only
`preferences.json`: its user renames a backup back to use it. Reading a newer minor's settings is a separate decision,
not made.

MIDI preference records have one writer, EngineApp. Generic GUI preference
saves preserve the latest records on disk and ask the engine to persist its
current records; opening a dialog never replaces engine state from a snapshot.
Startup uses `loadMidiPortPreferences` to skip each malformed MIDI record while
retaining valid peers; a malformed input/output section is skipped as a whole.
`midi.outputs.get` / `midi.inputs.get` retain skipped names with structured
`midi.preferences` warnings (code, path, message, hint). A successful correction
clears that record's warning. Skipped raw JSON records (and malformed whole
sections) are retained and written back unchanged on unrelated edits, including
writes to a new preferences file. An explicit correction merges into the raw
object, preserving its other fields, then validates strictly before replacing it.
Command edits still use strict validation.

Hardware routes preserve scalar-rate and mix-only edit paths. Route, source
and VCA mute values are part of the atomic mix publication (in every session
since ADR-0093); the callback updates its hardware gate consumers without a
plan compile.
Fader edits retain the running plan and prepared audio history.

### The FX chain's MIDI bus (ADR-0068)

A track's chain carries a MIDI bus (`midi_bus.cpp`). Per block, before the
note target's releases, `beginMidiBus` decides which note-output stages are
on the bus: running in this executor, processing and not bypassed (a stage
that then fails outputs nothing but stays on the bus). Part −1 is the
pre-FX stream; part *q* is the *q*th note-output stage. A part reaches
stage *k* when it comes before *k*, is on the bus, and no replacing stage on
the bus lies between it and *k*; the pre-FX part reaches *k* when no
replacing stage on the bus lies before *k*. What a stage holds of a part
that no longer reaches it ends at the start of its block (the note target's
with its own releases); if the pre-FX part no longer reaches the note
target, its pre-FX voices end and it no longer hears that part.

After `finishTrackMidi`, `fillMidiBus` copies `midiPreFx` onto the bus,
marking a take's own notes and controllers (`midiChain`) so no plug-in of
the track hears them; sends still do. Before each stage, `feedMidiBus`
gives a note-accepting stage the bus, notes under `kBusNoteBase` + their
bus id (the note target skips the pre-FX part, which it already heard under
its own ids), merged by time with its automation and releases. A track whose
`chain_midi` is `instrument` (`Node::midiBusOn` false) has no note-output
stages on the bus and feeds no stage: only the note target hears the
track's MIDI, and post-FX equals pre-FX, as before 2.10. A plug-in with
note ports and no audio ports runs in either mode, the audio passing it. After each
stage, `stageMidiBus` turns a note-output stage's output into bus events:
notes (and MIDI 1.0 note messages) take its voice slots, keyed by the
plug-in's `(port, channel, key, note id)`, with an end before an onset on a
held address and CLAP wildcard ends; all-notes-off ends that channel; other
MIDI passes. It also records, per output port below 8, the channels the
stage's MIDI moved a controller (not a channel mode message), pitch bend or
pressure on; Reset All Controllers clears one. A block whose output is lost
(`OutputEvents::lost`: a late or skipped bridged block, or a failed bridge,
ADR-0088) ends every held voice at frame 0 and sends Reset All Controllers
on each recorded channel, as the stage's output; `carryBus` keeps the
record with the held notes. Replace makes that the bus; merge interleaves it after the
bus's own events at each frame. `finishMidiBus` writes `midiPostFx`: first
the releases of post-FX voices of parts no longer reaching the end, then
the bus; `midiPostHeld` follows it, and post-FX send restarts read its
block-start copy. Every listening stage tracks its voices (`trackVoices`),
bus notes marked `kBusNoteOrigin`.

Bus ids are the pre-FX slots, then `kBusVoices` (256) per note-output
stage. Adoption (`carryBus`) keeps a continuing stage's held notes and maps
their old bus ids to new ones (same instance and FX id), maps pre-FX ids
by the pool's remap, carries `midiPostHeld`, remaps listening stages'
bus notes (else they end, at the start of the next block, also on a
listener other than the note target) and lets post-FX sends carry their
voices by the same map. A seek ends the pre-FX part's post-FX notes and what other stages
than the note target hold of it.

A note-output stage's sink holds `kBusOutputEvents` (1024, the bridge
arena's room). Bounds: a stage adds 256 voices and `kBusStageEvents`
(2 × 1024 + 2 × 256) events per block to the bus; `midiPostWindow` and
`midiPostRate` add two blocks and one of that per stage, plus a release per
bus voice; post-FX taps size their queues and voices from them. A
listening stage reserves what can reach it: the pre-FX part unless it is
the note target, and the stages ahead of it; a chain without note-output
stages reserves what it did before. A stage with note ports and no audio
ports (`midiOnly`) runs with no audio buffers; the audio passes it through
its compensation delay, and it is never oversampled. Bus notes keep the
chain's input time; post-FX taps add `latChain`.

## Local crash diagnostics (ADR-0083)

The frontend supervisor and terminal handoff contract is in `diagnostics.md`.
A terminal audio fault never returns to DSP, saves or destroys engine state.
Normal RT rules stay absolute; diagnostics-only hangs never kill the engine.
