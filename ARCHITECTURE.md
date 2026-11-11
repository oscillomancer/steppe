# Steppe architecture

Steppe is a Linux-first DAW in which an LLM is a first-class operator, plus a
suite of stock plugins designed to be programmed by LLMs, without making the
product worse for the human who uses it directly. Its workflow reference is
REAPER (`docs/research/reaper-ux.md`). This document is the map; the
decisions behind it are in `docs/decisions/` (ADRs) and the contracts in
`docs/specs/`.

## 1. The shape of the system

```
          human (GUI)       Lua scripts        external tools       LLM agents
               │                 │                   │                  │ MCP (stdio)
               │                 │                   │ JSON-RPC 2.0     ▼
               │                 │                   │ (Unix socket)  mcp adapter
               ▼                 ▼                   ▼                  │
        ┌──────────────────────────────────────────────────────────────────────┐
        │  COMMAND BUS  (registry · validation · dispatch · batch/transactions)  │
        │  one typed, versioned, JSON-schema'd command set; events out           │
        └───────────────┬──────────────────────────────────────┬───────────────┘
                        │ edit: pure (Session, Args) → Session  │ query / action
                        ▼                                       ▼
        ┌────────────────────────────────────┐     ┌───────────────────────────┐
        │ SESSION MODEL (model thread)        │     │ services: render, analysis │
        │ immutable versioned tree, stable    │     │ (core-analysis), file I/O, │
        │ IDs, branches, per-branch undo      │     │ plugin scanning, optimizer │
        └──────────────┬─────────────────────┘     └───────────────────────────┘
                       │ compile version → RenderPlan (immutable graph)
                       │ wait-free pointer swap
                       ▼
        ┌────────────────────────────────────────────────────────────────────┐
        │ ENGINE (RT)  audio callback → graph executor → worker threads        │
        │ anticipative render-ahead for non-live tracks; PDC; automation      │
        │ plugins: CLAP, VST3 (in process / shared bridge / dedicated process) │
        └───────────────┬────────────────────────────────────────────────────┘
                        │ AudioBackend: JACK · PortAudio (native) · dummy · offline
                        ▼
                     hardware
```

Project edits go through the command bus. The GUI, scripts and agents use
the same validated edits, undo history and audit log. Window layout,
selection and application preferences are separate UI state; local actions
such as opening dialogs are not all engine commands.

## 2. Components

| Layer | Component | Role | Spec / ADR |
|---|---|---|---|
| base | `libs/core-base` | `Error`, `Result<T>`, `SUPPLE_RT`, RT primitives | ADR-0003, `docs/specs/core-base.md` |
| data | `libs/core-json` | JSON value, precise-error parser, canonical writer, JSON Pointer, merge patch | ADR-0009, `docs/specs/core-json.md` |
| data | `libs/core-schema` | typed schema DSL: JSON ⇄ C++ binding, validation with hints, JSON Schema export | `docs/specs/core-json.md` |
| params | `libs/core-params` | semantic parameter definitions, units, curves, text I/O, normalization | ADR-0010, `docs/specs/core-params.md` |
| patch | `libs/core-patch` | patch/preset documents: schema, validate, merge, canonical form, prose describe | ADR-0009, `docs/specs/core-patch.md` |
| music | `libs/midi`, `libs/notation` | MIDI model + SMF I/O; the text notation compiler/decompiler | `docs/specs/notation.md` |
| time | `libs/timeline` | tempo and meter maps, ramps, bar/beat conversion | `docs/specs/session-model.md` |
| audio | `libs/audio-file`, `libs/core-dsp` | file I/O, resampling; shared DSP blocks | — |
| audio | `libs/console9` | Console 9 encode/decode, fader/pan, deterministic dither; reference null test | ADR-0019 |
| perception | `libs/core-analysis` | loudness (BS.1770/R128), spectral, harmonic, envelope, stereo, comparison, masking; compact reports | `docs/specs/core-analysis.md` |
| tuning | `libs/optimize` | CMA-ES for the patch optimizer | `docs/specs/optimizer.md` |
| control | `libs/command-bus`, `libs/control-protocol`, `mcp/` | registry and dispatch; JSON-RPC over Unix socket; MCP adapter | ADR-0007, `docs/specs/command-bus.md` |
| scripting | `libs/lua-bus` (vendored PUC Lua 5.4) | sandboxed Lua scripts on the bus: generated bindings, `q.batch` transactions, `q.on` subscriptions; `script.run` | ADR-0006 |
| hosting | `host/clap-host` | CLAP and VST3 hosting, the semantic-params extension, the bridge process (`host/clap-host/bridge/`) | ADR-0013, ADR-0017, `docs/specs/semantic-params-extension.md` |
| sandbox | `host/sandbox`, `host/analysis-commands` | headless plugin sandbox (library, CLI, MCP); analysis commands shared with the engine | `docs/specs/sandbox.md` |
| engine | `host/engine` | session model (`host/engine/model/`), commands, render plans, real-time engine, JACK and native PortAudio backends, renders | `docs/specs/session-model.md`, `docs/specs/engine.md` |
| plugins | `plugins/*` | stock plugins (template-derived, or DSP scripts in `plugins/<name>`) + shared extension code and the script suite | ADR-0011, ADR-0047, ADR-0032 |
| app | `daw/gui` | the GUI application | ADR-0005, `docs/specs/gui.md` |
| agents | `skill/` | bundled LLM skill: generated reference + guides | — |

Runtime libraries under `libs/` do not depend on host, daw or plugin products.
Stock plugins share the MIT libraries and template infrastructure; hosting
probes and conformance tests also link host libraries.

## 3. Threads and real-time safety

- **Audio (RT) thread** — the backend callback. Executes the current
  `RenderPlan`: reads inputs, runs live-path nodes at device buffer size,
  consumes render-ahead buffers for non-live paths, writes outputs, posts
  meters into SPSC rings. Never allocates, frees, locks, blocks, logs, throws,
  or touches files. Entry points are annotated `SUPPLE_RT` and every audio-path
  test also runs under RealtimeSanitizer.
- **Native MIDI receive threads** — CoreMIDI's client receive thread or WinMM's
  per-device callback publishes timestamped short messages into preallocated
  per-port SPSC rings. PortAudio drains/sorts/rebases them into engine chunks;
  no receive/audio allocations or locks (ADR-0042). The existing native device
  monitor reports topology as an advisory without restarting audio; it recycles WinMM
  SysEx discard buffers outside the receive callback.
- **Native MIDI sender thread** — audio publishes bounded events into one
  preallocated SPSC queue per output. CoreMIDI schedules host-timestamped UMPs;
  WinMM waits on a process-owned high-resolution timer and sends when due.
  Device calls and SysEx buffer ownership stay outside audio (ADR-0069).
- **Worker threads** — execute graph partitions in parallel (lock-free work
  distribution with atomic dependency counters) and anticipative render-ahead
  for tracks that are not live (not armed / not input-monitored), with larger
  internal blocks, into lock-free ring buffers the RT thread consumes. DSP
  tasks follow the RT rules, with the background-only bridge exception below.
  The background pool serves anticipative and offline work. A separate live
  pool helps the callback with a fork-join per segment (ADR-0024). Every
  executor runs each node, plug-ins included, with FTZ and DAZ set and the
  thread's MXCSR restored afterwards, so a node computes alike on any thread,
  in real time and offline; owned audio threads prefault their stacks before
  their first job (ADR-0025). On Linux, under a real-time callback the live helpers
  share its priority, while the renderer, the background helpers and bridge
  audio threads run SCHED_FIFO 10, below the kernel's interrupt threads
  (ADR-0026). The renderer waits until the next stopped block is due or
  the callback has consumed enough buffered audio for another block.
  Early plug-in requests wait at least 1 ms after the last stopped block.
  Plan/transport changes and ownership releases also wake it. Safety waits
  are half the effective render-ahead span while filling playback buffers,
  otherwise 1 s (ADR-0028). Offline work stays normal.
- **Model thread** — the only writer of the session. Runs the command bus,
  compiles versions into `RenderPlan`s, reclaims retired plans, services
  plugin main-thread callbacks (CLAP's "main thread" is this thread), owns
  plugin GUIs in-process. In the GUI app it is the UI thread; headless it runs
  an event loop for the control socket and timers.
- **I/O threads** — recording writers (preallocated buffers and crash-readable
  WAV files; overflow is counted and missing audio is padded with silence), peak building, media hashing,
  analysis jobs, renders. Playback media is decoded and resampled into an
  in-memory cache when a plan is compiled; a background, disk-backed cache is
  planned.

The headless platform layer (ADR-0035) preserves these Linux mechanisms and
adds macOS address waits and pipes, and Windows address waits and event handles.
`Waitable` carries borrowed native readiness sources through ModelLoop and MCP;
new embedding code uses typed sources instead of assuming integer descriptors.
On arm64 the per-node guard sets FPCR FZ and restores the complete register.
Platform directory roots come from `supple::paths`. Native device audio uses PortAudio CoreAudio/WASAPI/ASIO (ADR-0038); its
control-side monitor wakes the owner for device recovery. Device callbacks keep
the same chunked, allocation-free engine contract as JACK. Owned helpers apply
MMCSS Pro Audio or device-derived Mach time constraints on themselves before
work, through a startup barrier; on macOS the live helpers then join the
device's CoreAudio workgroup (ADR-0038). Native GUI embedding is implemented
(ADR-0040); real-device validation remains opt-in.
Native in-process hosting and supervised scanning are separate from that bridge
(ADR-0036). clap-host owns a reusable ChildProcess tree/capture helper and
a standalone scanner. The bridge uses that helper on every platform for
explicit control-resource inheritance and process-group/Job cleanup (ADR-0037).
The native headless engine/dummy backend and sandbox build independently of
Linux JACK and its hardware benchmark. Audio files use POSIX streams or wide-path
Windows handles with delete sharing, exclusive recording creation and atomic
render publication; recording writes remain on the I/O thread.

`core-base` provides `supple::MainThreadWake`, a coalesced notification relay
(ADR-0029). Producers publish atomics and wake an address waiter; its parked
non-RT thread signals a native `WakeEvent` (eventfd on Linux, a pipe on macOS,
an event handle on Windows). The bridge uses it for plug-in main
requests. EngineApp subscribes its local instances, engine, backend and jobs to
one relay that outlives all producers. Headless socket/MCP loops wait on that source,
local plug-in descriptors and the nearest monotonic service deadline (ADR-0030).
Audio callbacks notify for published work/state transitions and one-shot output
consumption targets, not unchanged blocks. Job/post notifications run outside
locks; service flushes events before Lua delivery. The GUI shares one parked
native-waitable watcher (service_watcher; ADR-0039) for the app relay, local plug-in descriptors and ModelLoop wake
source (including signals). SDL waits until those sources, input, or the nearest
app/panel deadline. Playback frames and transport counters stop with transport;
meters animate only while moving, and arm one activity wake at rest. Input and
commands arm a coalesced UI sidecar check; writer completion posts to main.

Model → audio: a `RenderPlan` with an immutable graph, swapped with an
atomic pointer; level and pan edits published into its mix slots (ADR-0060);
parameter changes and transport commands via bounded SPSC queues. Audio →
model: two kinds of channel, never confused:

- **Reliable, coalescing** channels for anything that is state: parameter
  changes made inside plugin GUIs, plugin "state dirty" and "latency changed"
  notifications, recording progress. Implemented as atomic latest-value
  slots plus dirty bits (per parameter, per node), so they cannot overflow:
  intermediate values may be coalesced, the final value never lost. The
  model thread drains them and turns them into versions (undo steps).
- **Lossy** channels for telemetry only (meters, scopes, analysis frames):
  SPSC rings that drop the newest item on overflow and count drops.

The device callback never blocks or calls the synchronous bridge. ADR-0013
sanctions a narrower exception for background rendering: bridge calls wake a
child with a futex, spin briefly, then wait with a bounded failure deadline,
inside `supple::rt::ScopedSanctionedSyscall`. This exception does not authorize
waits or synchronous bridge calls in the device callback. ADR-0024 additionally
sanctions `supple::rt::wake()`, a nonblocking private futex wake for live
helpers. ADR-0028 extends that same nonblocking wake to the renderer;
no callback waits are allowed. The callback executes ready nodes and joins
using atomics only; helpers park between segments, and every helper that
claimed the job must depart before plan state is reused. Unclaimed late
helpers cannot access retired jobs and need not be awaited. A late in-flight
node makes the cycle late; no output is discarded. The background pool
currently uses atomic wait/notify between jobs and when no task is ready.
Participation is bounded by graph dependencies and departure is joined
explicitly (ADR-0023); this pool does not participate in live callback execution.

**Main-thread budget.** The main thread is the UI thread, the model writer
and CLAP's plugin main thread (plugin GUIs require it). Render/analysis jobs,
browser rescans, peak building and recording writes use workers and post
completion back. Plan compilation and its media decoding/resampling, Lua
execution, and some project/file operations still run synchronously on the
model thread. Lua has an instruction budget; time inside commands is excluded.
The target of no synchronous UI-thread task over 2 ms is a responsiveness
goal, not a bound on every operation (`docs/benchmarks/gui.md`, `STATUS.md`).

**Event delivery to clients.** Every event carries a per-topic sequence
number. A subscriber whose queue overflows receives one `event.dropped`
notification with a count and re-synchronises with a snapshot query — the
only case where a client reads state instead of receiving it.

## 4. Session model, undo and branches (ADR-0008)

The session is an immutable tree with structural sharing. Each edit command
or batch produces a new **version**; a **branch** is a named ref to a version;
**undo** is per-branch and linear. Objects have short stable IDs (`tr_12`,
`it_40`, `fx_7`) from per-kind counters. Agents work on
`agent/<agent>/<topic>` branches; the human reviews an object-level diff,
A/B-compares offline renders (deterministic, sample-aligned, instant
switching) or live-reconciled graphs, and accepts (fast-forward or replay of
the branch's logged invocations onto `main`, with ID remapping and conflict
reports) or rejects.

The engine never reads the model directly; it runs compiled plans. Plugin
instances are reconciled against model versions: unchanged instances are
kept, changed state is loaded, removed ones are torn down on the model
thread.

## 5. The command bus (ADR-0007)

Commands are declared once in C++ with typed argument structs bound through
core-schema. Each has a dotted name (`track.add`), version, kind
(`query`/`edit`/`action`), docs, argument/result schemas with units and
examples, error codes with hints, and an MCP exposure class. Examples double
as tests. The registry supplies MCP tools, Lua bindings, the GUI's public
command actions and protocol dispatch at runtime. The skill's command
references, MCP tool schemas and Lua annotations are generated and committed,
with drift tests.

Agents are expected to use `batch` (atomic, one undo step, labelled). Object
references accept IDs or selectors (`{"name": "Bass"}`), resolved and logged
as IDs. Validation reports every problem at once, each with a JSON Pointer
path and a concrete fix.

Events (`session.changed` with per-object change lists; rate-limited
transport/meter/analysis topics) replace polling.

## 6. Plugins

### Hosting

CLAP (including `com.suppleaudio.semantic-params`) and VST3; LV2 is not
supported. Isolation modes per plugin instance, REAPER-style: in-process,
shared bridge process, dedicated process (ADR-0013). Bridged plugins
exchange audio and events with the engine through shared memory. The
caller (the anticipative renderer, or an offline render) writes the block's
input, wakes the bridge and waits for its output up to a deadline; a bridge
that dies or misses the deadline fails the plugins it hosts (one in a
dedicated process, every instance in a shared one: silence and an error
event) and the session survives. Bridges isolate crashes of ordinary plugins;
they are not a security sandbox against hostile ones. Synchronous bridge calls never run in
the device callback, so a bridged plugin on a live (monitored) node is
passed through with a warning; a pipelined mode for live nodes is future
work.

Linux keeps its memfd/socketpair/shared futex/pidfd path. macOS transfers an
unlinked shm_open descriptor, uses shared address waits and pipe notifications,
and observes child exit with kqueue. Windows duplicates unnamed mappings,
auto-reset request/done events and a manual-reset notification into an owned
Job child; overlapped pipe control preserves absolute deadlines. The native
launch adapter reuses the scanner's ChildProcess inheritance/lifetime helper.
All platforms publish through the shared atomic words before signalling and
recheck before parking; Windows process-local address waits are not used for
bridge audio. These mechanisms retain the background-only wait exception.

**Runtime latency changes.** CLAP only lets an active plugin change latency
via `request_restart`. Model-thread service first publishes a plan that takes
the instance out of both audio executors and passes its stage through with
compensation. Once the executors have adopted that plan, the instance restarts
with its state retained. A changed latency triggers a model-thread plan rebuild
with new delay compensation; plans are adopted at audio boundaries. Mix-control
smoothing and Console 9 mode crossfades use 10 ms; structural plugin changes do
not yet have crossfades. Offline renders are
unaffected (plans are fixed per render). Full contract: `docs/specs/engine.md`.
Plugin GUIs embed as native X11 children, NSViews or HWNDs (ADR-0040).
Windows/X11 bridge editors use the same parent across processes; Cocoa bridges
own floating NSWindows because NSView pointers are process-local. Native bridge
main loops service UI input alongside IPC/timers; SDL pumps local editors. The
separate native DAW GUI port supplies its shell and shared service watcher.

### Stock plugins (ADR-0011)

Instances of Supple Audio's plugin template, renamed per its identity
checklist and built as external projects (`plugins/AGENTS.md`). Each: SDK-free DSP core; a core-params parameter table (physical
units, curves, tags); a core-patch JSON patch format that is the primary
interface; the semantic-params extension for hosts that understand it,
standard CLAP params/state for those that don't; seeded randomness;
real-time and denormal safe; a GUI that reflects the patch structure; a
compact prose `describe()` of the current patch.

Newer stock plugins are DSP scripts (ADR-0047): one script each in
`plugins/<name>/`, compiled as C++ into a plugin of its own from the
same template and the script suite, with the same patch, semantic and
editor contracts.

Value domains (ADR-0010): physical units on the bus and in patches;
normalized curve positions as the CLAP value for generic hosts; DSP values
inside. core-params owns every conversion.

## 7. Perception (core-analysis)

Offline, deterministic measurement of rendered buffers at full resolution:
loudness (integrated/short-term/momentary LUFS, LRA, true peak), spectral
(centroid, rolloff, flatness, flux, named band energies, smoothed long-term
spectrum), harmonic (f0, partial amplitudes, inharmonicity, HNR), envelope
(attack, decay, crest factor, PLR, transient density), stereo (correlation,
width, balance, mono loss), comparisons (per-descriptor deltas against a
reference or a target profile; masking between two signals). Output is a
compact JSON report (a few hundred tokens by default, more on request) and a
matching one-paragraph text summary. No audio model ever "listens".

## 8. Agents

- **MCP**: in-process (sandbox/engine serve stdio) or proxy (`<slug>-mcp`
  to a running engine's socket). Most commands are MCP tools of their own;
  `describe`, `exec` and `query` reach the rest.
- **Branches first**: the skill teaches agents to propose on a branch and ask
  the human to listen. Every agent action is in the command log and undo
  history with its origin.
- **Division of labour for sound design**: the LLM picks structure
  (modules, routing, notation); the optimizer (CMA-ES, multi-descriptor
  loss) tunes continuous parameters against a reference or target profile.
- **The skill** (`skill/`): generated reference (commands, plugin schemas,
  notation) + hand-written guides, progressive disclosure, drift-tested.

## 9. Formats (ADR-0009)

Strict JSON with a `"schema": "supple.<type>/<major>.<minor>"` first member,
canonical serialization (byte-identical for identical content), ID-keyed
collections, RFC 7396 merge patches, strict validation with "did you mean"
hints. The MIDI notation is its own line-oriented format with a canonical
form. Project files follow the same rules (`docs/specs/session-model.md` §5).

## 10. GUI (ADR-0005)

SDL3 supplies native windows on Linux, macOS and Windows. Linux uses X11
(XWayland on Wayland desktops) so third-party plugin GUIs embed as X11
children. A custom retained-mode toolkit drawn with NanoVG on OpenGL,
sharing rendering technology with the plugins' editors; REAPER's workflow
model (arrange, TCP, mixer, FX chains, routing matrix, MIDI editor, render
dialog, actions list with bindable keys and macros, customizable toolbars
and menus, themes) with Steppe's own visual identity. UI work never stalls
audio: the UI reads model versions and engine telemetry rings; it cannot
block the RT path. Agents work through the MCP server from an external
harness; the app has no built-in agent (ADR-0043).

## 11. Quality gates

RealtimeSanitizer on audio-path tests (a self-check proves the gate fires);
ASan with UBSan, and TSan presets; golden renders with tolerance-aware null tests;
opt-in fuzz targets for JSON, notation, command-bus inputs, projects and history;
plugin validators;
benchmarks tracked in `docs/benchmarks/`; the license gate on every build.
Definition of done: code, tests, docs, `STATUS.md`, no license issues, and
review by someone other than the author. Nothing is "done" that was not run.

`STATUS.md` lists what works today, the known limits and what is planned.

The DAW windowing uses SDL3 X11/Cocoa/Windows and OpenGL core
(3.3, macOS 4.1), with native hidden-window render tests and Linux-only golden
references (ADR-0039). SDL density/display scale maps design units to pixels.
The UI's primary modifier maps to Command on macOS.
Native editor embedding uses typed X11/NSView/HWND parents (ADR-0040); Cocoa
bridges own floating windows, while local and Windows bridge editors embed.
The shared GUI watcher services native app/model and plugin waitables.
