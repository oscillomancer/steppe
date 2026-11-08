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
new plan (built incrementally from the previous one where possible, §1.6).
Its DSP state belongs to the executors, and strip, send and VCA levels and
pans arrive through its mix slots (ADR-0060, §1.6) without a new plan.

### 1.1 Nodes

- **StripNode** per track *(amended 2026-09-30, ADR-0019)*, stages by strip type:
  channel `inputs → [tap: pre_fx] → items/takes → inserts → mono/polarity →
  [tap: pre_fader] → C9 Channel (fader, pan, mute) → [tap: post_fader]`; buss
  `input sum → C9 Buss → [pre_fx] → inserts → mono/polarity → [pre_fader] →
  C9 Channel → [post_fader]`; master `input sum → C9 Buss → [pre_fx] →
  inserts → mono/polarity → [pre_fader] → plain gain/pan → its own items →
  [post_fader]` → outputs/renders/monitor chain (ADR-0057). Mono and polarity (ADR-0022) act on the
  main pair, ramped like mute, never between an encode and a decode. With
  `project.console9` off the C9 stages run linear (same gains and pan law,
  no curves, noise or dither; ADR-0019 §6). A channel's items play *before*
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
- **ItemPlayer** (inside a TrackNode): per active item on the track's lanes,
  per take: source reader (audio: disk-streamed decoded+resampled blocks
  from the media cache; MIDI: event generator from the source with playrate
  and loop), take FX chain, take envelopes, item gain/fades/crossfades.
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
  input uses `S - take.latency`. Track/take stage parameters, output-event
  timestamps and plugin transport use that input shift plus `stage.before`,
  at unity rate as well as under playrate. Fader controls use `S + chain`;
  send encoders use their source tap's shift, and unencoded key controls
  use their destination stage's. Device audio/MIDI are never read ahead.
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
- mixer targets (volume, pan, mute, send level/pan) interpolate per sample
  (gain ramps) and drive the C9 Channel (or the plain stage) — cheap and
  exact; Console 9 stages restart their seeded dither at every locate;
- plugin parameters receive CLAP param-value events at segment points and
  every `automation_resolution` samples inside curved/linear segments
  (default 32; always at exact point positions), in the parameter's CLAP
  domain (normalized for stock plugins, ADR-0010);
- write/touch/latch modes record from GUI/control input into the model on
  the main thread (reliable coalescing channel, ARCHITECTURE §3), creating
  one version per gesture. Capture timestamps are seconds; evaluation, entry
  guards, erase windows and stop holds convert them into each envelope's
  stored domain (seconds for `time`, tempo-map beats otherwise). Plugin-output
  recording follows the same conversion, and replay carries time-envelope
  points as explicit seconds through `env.points.set`.
- write holds (ADR-0061): once a pass has written a strip's or send's
  volume or pan, or a VCA's volume, the engine plays that target's static
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
  ends the pass that was playing. Mute, plug-in parameters and the master
  playrate are not held: their writes compile as before.

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

### 1.6 Incremental rebuild and plan swap

The compiler diffs the new version against the one the current plan was built
from (session-model §4). Unchanged nodes keep their plugin instances and
state (their DSP objects are moved into the new plan). Held timeline notes
carry only when the receiving instrument instance is retained. Route and parent
edge delay histories carry when source, destination, tap, kind and delay length
match; changed lengths start empty. New plugins are
instantiated and activated on the main thread before the swap; parameter
changes on existing instances become param events rather than rebuilds.
Returning to a version with neither patch nor state after applying saved FX
settings recreates the factory-default instance. The old instance remains alive
until both executors release its plan, as with a changed opaque state.
The swap: `std::atomic<const RenderPlan*>` exchanged by the model thread;
the RT thread picks up the new pointer at the next cycle start and
acknowledges via an epoch counter; the model thread destroys the old plan
(and deactivates removed plugins) only after the acknowledgement. Nothing on
the RT thread frees memory.

**Mix-value edits (ADR-0060).** A session that differs from the last one
`Engine::load` took only in strip `volume_db`/`pan`, send `volume_db`/`pan`
and VCA `volume_db` (and in the points of envelopes a write pass holds,
§1.4) is not compiled. The comparison copies each changed track, route, group
and envelope with those fields taken from the new one and compares the whole
object, so any other change (mute, solo, a name, an FX or a route's tap)
compiles. A send whose level moves to or from exactly 0 dB, or whose pan
starts or stops following its source, compiles: that decides whether it is
encoded. An unchanged session is a maintenance reload and compiles too. A
qualifying edit writes the values into the newest plan's mix slots (per
node, per route edge, per VCA; parent sends carry no values) between an odd
and an even sequence number. Each executor, at the start of a block
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

## 2. Real-time execution

### 2.1 Live and anticipative regions

A node is **live** when it consumes monitored hardware audio/MIDI or armed
audio input (including an input FX chain). Live classification propagates
downstream, including sends and the master. All other nodes are anticipative.
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
  Limit 1 processes it serially. Synchronous
  bridged stages pass through on live nodes, with a warning (§5).

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
- **Lifetime.** Threads end at `stop()`, so nothing is restored. CPU affinity
  is never changed: threads keep the inherited mask.
- **Bridges.** `host::setBridgeAudioPolicy` carries the applied background
  policy to bridged instances while the engine runs (normal otherwise): each
  bridge audio thread takes it when its processing starts (§5).
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
park between segments, with no post-cycle spin. Participation uses the chain
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
offline processing. Synchronous bridge stages still pass through, including on
helpers.

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

**Meters**. Per strip, the peak and the mean square of the strip's signal
**after the fader and after the pan law, before the Console 9 encode**
(`sum × faderGain × ` the stage's own `panGains` — what a user expects at
unity: a −6 dBFS item at 0 dB reads its own peak times the Channel pan law)
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
built, which commits their pages before any callback uses them. Owned audio
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
- As a render ends, each plugin's read-only parameters (meters, readouts)
  are read from its instance (`OfflineResult::meters`, by FX id); a
  `render.mix` keeps them on the main thread for `fx.meters {from:
  "render"}` until the next one. Other renders keep none.
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
  `skip_silent` or `if_exists: skip`. `render.dry-run` lists each file with
  `path` (where `if_exists` sends it), `exists` (its destination is already
  there), `skip: true` when `if_exists: skip` leaves it out, `seconds` and
  `outputs: [{path, format}]`. With no `operation` it plans the settings'
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
  writes a valid empty hash. Hash/read/sidecar failures leave it empty and emit
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
  Audio WAVs and placement sidecars remain recoverable; MIDI has no recovery
  sidecar. Forced termination cannot execute this shutdown path.
- **Recording admission**: an armed track with `record.mode: output` fails
  `transport.record` with `record.output_unsupported`, its `/tracks/<id>/record/mode`
  path, and a hint to select input mode. This check runs before any recording
  files are created, including files for other armed tracks. Existing projects
  with output-mode settings still load; output recording is not supported yet.
- **Recording**: RT thread writes input frames into large preallocated SPSC
  rings (≥ 10 s); an I/O thread writes them to WAV/RF64 files incrementally,
  rewriting the header every 5 s so a crash leaves a valid file up to the last
  flush; a sidecar per file records its track and start, and
  `project.recover` repairs the header to cover everything on disk and places
  takes a crash left out (ADR-0014); punch in/out and loop recording (one take per pass); MIDI recording
  into MIDI takes. Audio ring overflow becomes equal-length silence at its file
  position and is still reported; loop run positions remain in file-time units.
  Disk and periodic header errors stop only the failing target and retain that
  target's first error. The loop run table holds the initial run plus 4096 wraps;
  when full, capture stops adding input and returns retained passes with a
  structured `recording.loop_runs_full` warning for the commit. Sidecars require
  `supple.recording/1.0`, a track id, a positive finite sample rate, and complete,
  ordered loop runs beginning at file frame zero, with the loop range they were
  recorded in, or `[0, 0]` when seeks split the runs while no loop was active
  (an inactive loop range is never recorded); invalid sidecars return a
  structured error and must be skipped during recovery. Placement sidecars
  survive pending hash jobs (including finalized loop runs at stop); the
  background finalization adds an optional `hash` without changing their
  recovery contract.
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
- **Recording Note Out** (rack.md §6): an armed `midi_overdub` or
  `midi_replace` channel records every Note Out route delivered to it,
  after channel filtering/remapping, mute and compensation. Its selected
  MIDI input device may record alongside those routes. An instrument and a
  device record-input route are optional. Arming makes the destination and
  its Note Out sources live, including with monitoring off. Each node has
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

The bridge is native on Linux, macOS and Windows (ADR-0037). Linux retains
memfd, SCM_RIGHTS, shared futexes and pidfd behavior. macOS uses unlinked
shm_open descriptors transferred over socketpair, shared address waits/wakes,
a pipe notification and kqueue child exit. Windows uses pagefile mappings
and request/done auto-reset events duplicated into the child, a manual-reset
outward notification, overlapped duplex control pipe and owned process/Job
handles. The existing background/offline bounded-wait exception and failure
semantics apply on each platform. Native editor hosting follows ADR-0040;
the DAW shell is integrated separately on port-gui.

POSIX protocol 19 / Windows protocol 20 carry typed native editor parents
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
- Each instance's audio thread is named `audio-bridge`. On `StartProcessing`
  it takes the scheduling the host writes into the shared-memory header
  (`audioPolicy`, `audioPriority`; protocol version 10), which is the engine's
  background policy while an engine runs and normal otherwise (§2.2,
  ADR-0026). A renderer waiting for a bridged plug-in therefore never waits
  behind a lower-priority thread. Applying it is best effort: a refusal leaves
  the thread as it was.
- The current bridge is synchronous and runs only offline or on rendered-ahead
  nodes. A stage on a device-callback node (including a take FX stage) passes
  through with its compensated delay; the callback calls neither processing nor
  lifecycle methods of the bridge. The plan warning names the FX and asks for
  `isolation: in_process` to monitor through it. Pipelined live bridging is future
  work (ADR-0013); synchronous monitoring is not supported.
- Crash handling: a control thread watches the process (pidfd); on crash the
  instance becomes a **failed node** (silence; `fx.failed` event with the
  signal and last known state kept in the model), the rest of the session
  keeps playing, and `fx.restart` re-creates it from the last known state.
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
`engine.status` (q: backend, rate, block, load, xruns, anticipation
misses, meter drops), `engine.settings.set {render_ahead_ms,
worker_threads, automation_resolution, …}`.
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
`engine.error {reason, error}`.
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
  in-process for 16 instances.

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
`supple.capacity-run/2.1`: `xruns` is a JACK server counter delta or null for
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
- **Plan hand-off:** `RtHandoff<Plan>`. `acquire()` returns the previous plan
  to the main thread immediately, so the audio thread keeps its own
  preallocated list of the instances it is processing; on adoption it stops
  instances the new plan dropped and starts new ones, then stores the plan's
  generation; the main thread prunes the instance pool only once that
  generation reaches the newest published one (`service()`), so no instance
  is destroyed before the audio thread has stopped it. A plugin whose
  `start_processing()` fails is left out of processing (its stage passes
  audio through, delay-compensated, and `processErrors` counts it) and is
  reset as it refuses, so a retry that succeeds starts it clean; the next
  plan retries it. Per-stage event storage is reserved for the most events
  any block-long window holds, so a seek off the block grid never allocates.
  Plans share ownership of the session's TimeMap (a session version may be
  freed while its plan plays).
- **Transport:** play/pause/seek/shutdown are coalescing atomics (the latest
  request wins, none is dropped however many arrive between callbacks).
  Shutdown: `Backend::stop()` promises one final zero-frame callback on the
  audio thread (a manual driver makes it on its caller, which is its audio
  thread), and the engine stops every plugin there — no timeout, no stop from
  the main thread however long the device's period. Pause and
  seek `reset()` the plugins (no hanging notes); seek re-locates event and
  automation cursors and clears delay lines. Timeline output lags the
  processing position by the plan's output latency; monitored input incurs
  its own downstream path latency (ADR-0051).
- **Mixing decisions:** instruments add their output to the track's input;
  effects mix wet/dry against the input delayed by their latency; bypass is
  compensated (a delay of the stage's latency); the pan law applies to tracks,
  the master pans as a balance; mute silences the post-fader tap only; a
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
  wait (≤ 1 s) for two audio cycles so their answer is the new state;
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
  the pre-roll) are counted (`engine.status.anticipation_misses`). `stop()`
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
  or, for any plugin, by CLAP id or name (plain CLAP values). Semantic points
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
  Hardware audio and MIDI replace each other (`record_input` turns the MIDI
  route into audio, `record_midi` the hardware route into MIDI);
  `record_midi: null` clears MIDI alone. A rack Out keeps a MIDI input beside
  it, and such a channel records MIDI; without the MIDI input it records
  the Out's audio as a WAV take. A second armed channel with the same Out
  as its input records the audio in the same pass. A rack's Notes
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
  stages pass through on live nodes (§2.1), so a bridged instrument falls
  silent when armed. A node leaving the live region stays live while the
  transport plays (`keepLive`).
- **Smooth swaps:** a plan swap carries each strip's fader ramps and Console 9 state
  and each insert's applied wet level into the new plan
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
- **Not yet:** MIDI output ports, crossfades for
  structural changes (inserting or removing plugins). B1/B2 measurements:
  `docs/benchmarks/b1.md`, `b2.md` (tooling: `tools/bench/README.md`).

### Recording diagnostics (2026-10-04)

A committed recording may carry a `warnings` array with structured
`code`, `message`, `path`, `hint`, `track` and `file` fields.
`recording.loop_runs_full` is forwarded to `transport.stop` / pause callers
and the `record.warning` event while retained passes still commit.
`project.recoverable` and `project.recover` expose skipped invalid-sidecar
diagnostics in the same optional `warnings` shape (`record.sidecar`). Recovery
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
Missing/read-only parameters stop receiving events and produce a warning naming
the FX and parameter. Clear `ALL`/`AUTOMATIONS` suppresses only the matching
parameter's automation until a subsequent rescan or explicit engine load. Clear
`MODULATIONS` has no automation effect (the engine has no modulation lanes).
Envelopes remain in the model. A rescan after a clear in the same service batch
re-enables it; a clear after a rescan remains effective. Changes are heard at
normal plan-adoption boundaries, including the anticipative edit horizon.

`Engine::takePluginOutputs()` is main-thread only. Each record contains FX id,
plan generation, transport epoch, timeline sample, stream sample, the entire
`host::OutputEvent`, and idle/anticipative markers. Timeline coordinates subtract
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
   automation points. Reject removed FX and superseded plan/transport epochs.
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
   current values. Status exposes `dropped`, `voice_dropped`, and
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
`{requested, active, error?}` structure as memory_lock. Failures use
`engine.cpu_idle_latency`, path `/prevent_deep_idle`, with numeric errno and
a corrective hint. EACCES/EPERM explain that the account needs membership
in a group with device write access (commonly audio; policy varies);
ENOENT/ENODEV report that the device is unavailable. Acquisition failure
keeps intent enabled and audio usable; close failure records an error with
active=false because the descriptor has been released.

Tests inject open/write/close operations for all lifecycle and error paths.
The real libc write/close path is exercised against a temporary file through
an injected open. Automated validation never writes to the real device.
User-only acceptance: enable/disable and engine-stop/restart/exit cleanup
with the real device, plus repeated sparse/dense 64/128-frame sessions,
actual granted period, callback tails/xruns and power/thermal/fan behavior.
No latency benefit is claimed without those measurements.

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
queries direct the caller to engine.inputs. Selection errors use backend.device
and the relevant host_api/input_device/output_device path. Format/channel errors
use backend.config or backend.open, with a corrective hint. Duplicate matching
names are rejected. No numeric device index is persisted across restarts.

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
PortAudio exposes rejected messages and ring/period overflow through it and
`engine.status.midi_dropped`. SysEx/system messages are dropped and counted
per reported packet/buffer; driver-unreported SysEx loss cannot be counted.
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
  requested or render-lookahead value. Without an engine it is 1.0.
- `transport.playrate.set {rate?: number, preserve_pitch?: bool}` is an edit
  returning the same result. Rate is finite 0.25..4.0, and omitted fields retain
  their values. Empty arguments or unchanged settings make no version unless
  the call writes an automation point. GUI drags use `Origin.gesture` for the
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
stretch lookahead is respected. Each timeline ramps linearly from its current
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
and signal-path reachability. Member fader/envelope data never moves.

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
