# Status

Updated 2026-10-30. This file describes the present: what works, what is
known not to, and what is planned. Keep it current with each change; the
history is in git.

Steppe 0.6.0 runs on Linux (X11 or XWayland, JACK through PipeWire or a JACK
server) and is used for real sessions. Full local CI (`tools/ci/run-all.sh`:
license, format, dev, release, gcc, asan, tsan, rtsan) passes on
`main`.

The repository has on-demand Linux/macOS/Windows CI and portable presets
(ADR-0034), including stock-plugin CLAP validation.
Native headless services, hosting/scanning, bridge, stock wrappers, device
backend builds and GUI tests have passed Windows CI and macOS arm64
CI (ADR-0036/0037/0038/0039).
Windows executables run with the UTF-8 process code page, so non-ASCII
paths work whatever the system locale; the minimum is Windows 10 1903
(ADR-0044). Not yet confirmed by Windows CI.
The macOS preset uses AppleClang and system libc++; shared float/double parsing
uses vendored fast_float (MIT) on all platforms, including the suite's
self-contained source exports. Native waits/wakes, thread setup, control
protocol and standalone MCP/proxy have native coverage on both platforms
(ADR-0035).
Both native platforms enable and test audio-file I/O, recording recovery,
engine/CLI, dummy/offline rendering and sandbox. PortAudio builds CoreAudio on
macOS and WASAPI/ASIO on Windows; fake-device tests verify RT/offline equivalence
in both executors. Actual hardware trials remain opt-in and unmeasured. The
GUI gate is lifted (ADR-0039), with native editor embedding (ADR-0040);
the combined native DAW builds and hermetic tests pass CI on both platforms.
Portable libraries, the session model, stock DSP cores
and the script suite build independently of those services; see
CONTRIBUTING.md for the complete gate list. On Windows, a save blocked by
another program that holds the file open (no delete sharing) retries for about
a second, then reports which file and what to close.

The bridge port implements native macOS shared memory/address wakes and Windows
mapping/event handle transfer, notifications and process isolation (ADR-0037).
All platforms launch and clean up through the shared ChildProcess helper, with
explicit IPC inheritance and process-group/Job ownership. Linux retains its
memfd/futex transport and pidfd death watch. Bridge stderr retains plug-in
diagnostics; scanner stderr remains suppressed.
Linux bridge memfds are sealed against shrinking and growing after sizing;
their mappings stay writable. The bridge isolates ordinary plugin crashes,
not hostile plugins with the user's process privileges.
Bridge targets and portable fixture tests are enabled and pass native CI on
macOS and Windows. Engine fixtures cover shared/dedicated crash and deadline
failures, structured errors and restart ownership. Sandbox rendering verifies
exact isolated/local output through the existing host/render APIs;
plugin.load continues to load in process. Linux bridge, engine and sandbox
tests pass under RTSan and TSan, retaining the existing deadlines and
background-only processing contract.

Native host editor paths now select CLAP cocoa/win32 or VST3 NSView/HWND,
with typed full-width parent handles (ADR-0040). Windows bridge editors embed;
macOS bridge editors own floating NSWindows and service AppKit with IPC.
Hermetic editor tests cover partial-open cleanup, resizing and repeated teardown.
Native visual/focus/IME validation remains opt-in; combined DAW-side native
shell/editor integration tests pass CI. Cocoa bridge windows remember their place
per effect like other editors (their focus is not tracked for MIDI learn); they
use normal level, order above the DAW window without activating the
bridge, and are raised again when the DAW window regains focus. A display-scale
change (monitor move, system scale) is passed to open Linux/Windows editors, local
or bridged, CLAP or VST3, and resizes them; hermetic tests cover the rule, the
visual result is unchecked without a desktop with mixed scales. Windows child
keyboard behavior is plugin-dependent.
The Windows bridge initializes Per Monitor v2 DPI awareness before loading
plugins; the display-free startup regression awaits native Windows CI.
Windows control reads check their absolute deadline before consuming buffered
input too; its regression also awaits native Windows CI.

Evaluation harness fixtures now run on Windows as well as POSIX: executable
fixtures use Python, credential references can use Windows hard links, and
attempt cleanup uses native Jobs or POSIX process groups. Real sandbox/model
checks remain explicit opt-ins. Source-export player/factory/audio conformance
runs natively, including binary exports: Windows retains the module trailer;
macOS uses a bundle resource and ad-hoc signing. The lifecycle and thirteen
source/binary export conformance cases pass CI on Linux, macOS arm64 and Windows.
Failed MCP initialization closes the evaluation client's pipes and process tree;
its standalone unit fixtures also run before configuring a build. The new
cleanup regression passes locally on Linux; macOS/Windows CI is pending.
The offscreen mockup tool uses the shared native pipe-readiness helper and
closes its MCP streams. Its Linux transport fixture passes; native runs are pending.

Packaging (ADR-0048) has CMake install components, a relocatable Linux tarball
and user-space installer, an Apple Silicon app/dmg and Windows per-user NSIS
installer, with portable zip alternatives. The five helpers/programs, fifteen
stock CLAPs, agent skill, fonts and license texts ship together. Package CI is
manual and tests installed artifacts; optional release upload creates drafts
only, from all-platform runs at one commit; existing drafts reject a different
target revision. Linux package builds now use hidden static C++ runtimes (ADR-0049), preserving
third-party plugin runtime independence; the audit rejects shared C++ runtimes.
Both installers upgrade their own previous version and preserve unrelated
files; both preflight ownership before removing an old installation and remember
external-plugin opt-in. Removers refuse linked private payload directories;
tests cover upgrade collisions, directory links and icon-directory ownership.
Linux uninstall also preflights all ownership paths and refuses parent traversal
before deleting any files; the local installer regression covers malformed records.
Package CI checks installed native artifacts, upgrades and uninstall cleanup separately
from the native regression suite. macOS finalization keeps notices in bundle
resources and signs nested code before the app. Bundled SDL resolves package-manager
symlinks into private library files. Windows stages the resolved DLL set and
ships SDL/zlib notices; unrelated build-tree DLLs are rejected. The packaged
smoke starts the GUI offscreen from the filesystem root, as Finder does; a
windowed launch through Finder or a desktop launcher is checked by hand only. No paid signing or notarization is available; first-launch
instructions are in `docs/guides/installing.md`.

## What works

- **Licensing** (ADR-0033, ADR-0062): DAW code is GPL-3.0-or-later; stock
  plugins, their shared libraries and `modules/` (the rack runtime and the
  stock modules) are MIT.

- **Session and history** (`specs/session-model.md`, ADR-0008): immutable
  versions with stable ids, per-branch undo, branches with object-level diff
  and merge by replay (field-level conflicts, id remapping), A/B renders of
  two branches, a command log with origins, a history budget that squashes
  the oldest steps. Undo to an FX's initial unsaved settings restores its factory
  state while retaining the previous processing instance until plan retirement.
  Project format `supple.project/2.6` with migration from
  1.x, autosave, crash recovery of recordings, a per-project UI sidecar.
- **Track freeze** (ADR-0054): whole-channel pre-fader float32 capture, including
  take FX and auxiliary channels, with virtual latency and plugin retirement.
  Typed freeze/unfreeze, cancellable jobs, dependency-checked completion, retained
  media for history/branches/saves, and context-menu actions with frozen markers.
  The GUI starts the job without following it: no progress, no cancel, and a
  later failure is not shown (agents use `job.get`/`job.cancel`).
  Mixing stays live; captured edits and global timing require unfreeze. V1
  excludes external inputs, armed tracks, non-unity master rate and partial/buss
  freeze. Cache preparation currently uses MediaCache; a prepared-source service
  can replace that localized path. Seek/loop/random-plugin and external plugin
  asset limits are documented in the engine spec and manual.
- **Command bus** (`specs/command-bus.md`, ADR-0007): about 120 typed
  commands with JSON Schema, units, examples run as tests and structured
  errors. Context and decoder warnings reach successful replies in `warnings`,
  preserving command diagnostics consistently over sockets, MCP and Lua
  with compact per-command schemas and one shared diagnostic contract
  (ADR-0063); strict handler result validation is unchanged. `batch`
  transactions; events; the JSON-RPC socket; the MCP adapter
  (in process or as a proxy to the running app); Lua scripts on the bus
  (ADR-0006). Lua structured-error expansion has the same value/text budget
  as returned values, including repeated references to shared details.
  The skill's command reference is generated and drift-tested.
- **Engine** (`specs/engine.md`): deterministic offline renders with
  real-time output bit-identical to them; anticipative rendering for
  non-live tracks (ADR-0012) and a live region for monitored tracks
  (ADR-0014); a DAG worker pool with graph-bounded participation and idle
  parking (ADR-0023), a separate live callback fork-join pool (ADR-0024)
  with FIFO JACK helpers, a claim-based lifetime join that does not wait for
  unclaimed helpers, and an Automatic/limit audio-thread preference;
  an opt-in host-process memory-lock preference with effective status and
  actionable OS failures (bridges excluded; cleanup also drops plug-in locks);
  a separate default-off 20 µs CPU idle-latency request with close-based
  cleanup and permission diagnostics (real-device validation is user-only);
  audio threads named by role (`audio-render`, `audio-bg-N` and others);
  the renderer, background helpers and bridge audio threads at SCHED_FIFO 10
  below a real-time callback when permitted, with a warning and normal
  scheduling otherwise, reported per thread class in `engine.status`
  (ADR-0026);
  read-ahead plugin delay compensation (ADR-0051, with per-FX
  `pdc: ignore`): unrelated playback tracks no longer delay monitored input,
  fan-out stays aligned and arming does not change the timing solution;
  parallel FX groups; per-FX oversampling (2/4/8x); input
  FX on armed channels, monitored and recorded; plugins keep
  running while the transport is stopped; renderer deadline/event waits
  (ADR-0028); headless main-thread service driven by notifications and
  deadlines (ADR-0030), including MIDI controls, plug-in subscriptions,
  render-ahead output consumption, jobs, restart and autosave; smooth plan swaps (10 ms ramps);
  strip, send and VCA level and pan edits applied to the running plan without
  compiling, also once playrate scheduling has started (ADR-0060), and control
  drags sent once per frame rather than per pointer motion;
  every node, plug-ins and bridged plug-ins included, runs with FTZ/DAZ in
  every executor, and owned audio threads prefault their stacks (ADR-0025).
- **Strips and routing** (ADR-0019 to ADR-0022): typed channel, buss and
  master strips with Console 9 summing (switchable, level-matched); routes
  with taps, channel maps, mono and polarity; sidechain keys (an unrouted
  key deactivates a plug-in's key port or bus, so it can key from its own
  input); up to 128
  channels per strip with plugin pins and multi-output instruments; strip
  polarity and mono; the routing matrix (cells selected by click, Ctrl,
  Shift, row, column or all; a double click or Add sends; every control,
  the level fader and pan included, acts on the selection; sends' envelopes
  under their strip). Several masters, each playing to
  its own device outputs (a pair, one mono output or none; ADR-0056), in
  step with each other, picked from the master strip's out button; the
  engine opens every output of the device (up to 64).
- **Recording**: armed inputs to crash-readable WAV, input monitoring (needs
  the arm), MIDI monitoring and recording with overdub, loop recording into
  takes, punch, count-in, metronome. Armed masters print their output in real
  time onto a lane of their own (ADR-0057): the take is bit-identical to
  `render.mix` of that master in every executor; a master's items play after
  its fader, beside the live mix, with a lane mute, and are silent in the
  punch range while it records.
- **Editing**: items, takes, comping lanes, fades and crossfades with shapes,
  cursor splitting from the item right-click menu (shared with S) and an audio-only prior
  zero-crossing split (100 ms search, louder stereo channel, cursor fallback;
  agents use `item.split` with `snap: zero_crossing`),
  time stretch and pitch (Signalsmith Stretch), draggable audio warp
  anchors (including identity insertion at the edit cursor from the Item menu),
  transient detection, joint multitrack audio quantize and slicing,
  background `item.consolidate` for active audio with take FX baked and MIDI
  kept separate per track (one undo step, track processing stays live),
  item Export… (`item.export`: the heard take's raw source span as WAV, trims,
  splits and loops respected, or a MIDI item as a Standard MIDI File),
  MIDI file import (`item.add {midi_file}`, File > Import… and file drops),
  Mix to new track (background `item.mix-to-track`: selected items through
  their full strips, Console 9 decoded as a fresh buss hears them, onto a new
  muted channel, one undo step), an item right-click menu for everyday edits
  and the menu bar's Item menu for the rest (no shared entries),
  adjacent Consolidate items / Heal splits item-menu actions with shared engine
  eligibility, and non-rendering `item.heal` for compatible split/slice chains,
  retaining outer fades and full source maps while removing generated interior fades,
  markers, regions,
  tempo and meter maps, the item clipboard, a piano roll with velocity and
  CC lanes, an event list and a notation panel.
- **Automation**: envelopes on strip, route and plugin parameters,
  automation items with pools, take envelopes, touch/latch/write recording
  (a written level or pan is heard as the hand moves it, held over its curve
  until the pass ends, or for touch 500 ms after the last move: ADR-0061);
  automation points selected (click, Ctrl+click, or the arrange marquee,
  which takes items and points on any track at once) and moved, deleted,
  copied, pasted and reshaped together, across lanes, from the lane's menu or
  the keyboard.
- **Rendering** (`specs/engine.md` §3, ADR-0055): mix, stems, regions and
  the render matrix through one implementation; one set of render settings
  per project (`project.render`, schema 2.4, undoable), resolved over the
  user's render default and the built-ins (`render.settings` shows each
  member's layer); per-user presets (six factory ones, read-only);
  file-name patterns with `{version}`, `{branch}` and subfolders, and
  overwrite / number / skip / error for existing files; project, time selection or
  custom bounds, tails, seamless loops, region handles, trim of silence;
  loudness or peak normalization (only down, or one master gain for every
  stem) and a true-peak limiter with or without it; a mix of several
  masters, a file each (ADR-0056); up to four outputs per
  file: WAV (16/24-bit with seeded TPDF or noise-shaped dither, or float,
  RF64 beyond 4 GiB), AIFF, FLAC, MP3 (LAME, gapless header) and Ogg Vorbis;
  tags, a cover, BWF timecode and loudness and embedded markers where the
  format holds them; loudness and true-peak checks on each file as written
  (lossy files decoded first), with an optional text report; render jobs and a queue that pins version and
  settings; per-file analysis, branch and version; `branch.create
  {version}` starts a branch at a render's mix.
- **Plugin hosting** (ADR-0013, ADR-0017, ADR-0018): CLAP and VST3,
  in-process or isolated in shared or dedicated bridge processes (a crash
  fails the instances in that bridge), supervised scanning with a cache, editors
  embedded in process and from bridges, editor windows that remember their
  place per effect, plugin state saved with the project. Bridge main loops
  park until socket/descriptor/request activity or the nearest plug-in timer;
  CLAP/VST3 requests use core-base's audio-safe coalesced wake relay
  (ADR-0029), and timers run without an open editor; remote changes/death
  wake the app without editor or audio idle polling (ADR-0031).
  Fixed-size CLAP parameter strings and VST3 bus/parameter strings are bounded
  even when a plugin omits their null terminators; Linux regressions pass,
  with native macOS/Windows validation pending.
  VST3 parameter metadata preserves the automatable flag in the shared CLAP
  flag representation; its Linux regression passes, with native CI pending.
  Sandbox rendering rechecks negotiated bus widths after activation, refusing
  VST3 instances that switch to mono before audio starts. Local and bridged
  Linux regressions pass; native macOS/Windows validation is pending.
  CLAP descriptor dispatch ignores registrations removed by an earlier
  callback in the same readiness snapshot, as VST3 dispatch already does;
  the Linux regression passes and macOS validation is pending.
- **Modular racks** (ADR-0062, `specs/rack.md`): racks of modules and
  cables on a channel, in the project (schema 2.7) with undo and branches.
  Out modules are channel inputs, In modules take sends, Notes modules play
  a track's MIDI or a live input, Transport gives the song's clock, plugin
  modules host CLAP/VST3 plug-ins (instruments played from pitch and gate
  cables). 83 stock modules (`modules/stock`: oscillators, filters,
  envelopes, sequencers, generative and utility modules, effects) compile
  as scripts and as C++ alike. Each rack is a node of the render plan on a
  32-frame grid; module automation holds across host block fragments.
  Real-time output equals offline renders, and the audio path passes
  RealtimeSanitizer. Engine-owned `module.copy` / `rack.paste` retain modules,
  internal cables (including plug-in parameter modulation), learn bindings
  and copy-time live plug-in state across cuts and racks, as one paste undo
  step; unavailable types or plug-ins refuse the paste without changing the
  project. Every running module publishes a lock-free per-output peak across
  all poly channels without a telemetry watch; `rack.levels` and the main-thread
  `EngineApp::moduleOutputLevel` accessor expose it independently of visibility.
  Stopped readings are zero and both executors preserve RT/offline equivalence.
  Note Out (`notes_out`) plays any channel's instrument through MIDI routes,
  sharing Capture's gate and pitch/velocity rules, with poly voices, channel
  filtering/remapping, fan-out, compensation and exact RT/offline timing.
  Armed MIDI destinations record these routes, with or without an instrument,
  alongside device MIDI, using the recorder's loop, punch and overdub rules.
  Armed channels also record their rack Out audio input as stereo WAV,
  including input FX, loop/punch takes, recovery and undo.
  Sends into rack In ports preserve the tap at the route's level and pan,
  without Console 9 trim, with the console enabled or disabled.
  Rack audio and Note Out routes now participate in read-ahead PDC, including
  oversampling and runtime latency changes (ADR-0064); `rack.get` reports
  the node and each output latency (default reads prepared timing, full can
  probe; unprepared values are null). Routed audio and Note Outs set the node;
  forward paths in loops count after removing scheduler back edges. Oversampled
  FX service compares raw latency separately from prepared stage latency.
  Plug-in modulation cables accept numeric
  CLAP parameter ids (`fx_3.12`) as well as semantic keys, including plug-ins
  without semantic metadata; both survive project and preset reopening.
  Stale saved targets warn instead of blocking project opening; new cables
  validate the actual FX state and require canonical numeric ids.
  Agents patch with `rack.patch` (named
  modules and cables in one step, laid out by signal flow and labelled by
  name; `rack.arrange` tidies a rack) or `module.add`/`cable.add`, hear a patch
  with `rack.probe` (pitch, levels, gates, notes, CV rates from an offline
  render of a copy), print its notes to a MIDI item with `rack.capture`
  (or `async:true` for a cancellable snapshot job with one undoable completion), write and edit library modules (`module.write`, hot
  reload in the running engine, `module.fork`), keep patches as rack
  presets (`rack.preset.*`, carrying the user's modules' source; projects
  keep copies of theirs too), start from factory patches that ship with the
  app (`modules/patches`, listed with `factory: true`; a test plays each), automate knobs (`env.add {module}`) and learn
  MIDI controllers on them (`module.learn.*`). The agent guide is
  `skill/guides/rack.md`; `skill/generated/rack-modules.md` lists every
  built-in module. Linux only so far: native macOS/Windows CI has not run
  on racks.
- **GUI** (`specs/gui.md`): arrange view, track panel, mixer, FX chains and
  the plugin browser, routing matrix, MIDI editor, render dialog (the
  project's render settings with dropdowns, foldable sections, presets, a
  user default, up to four outputs, delivery checks in its results, and
  File → Mix as a job), history dock with
  branches, actions list with key bindings and macros, editable
  menus, toolbar and mouse modifiers, screensets, two palettes in dark and
  light, preferences, and tap tempo in the tempo form. Help exports the bundled
  skill as a copy or link, shows this copy's MCP commands and JSON, and registers
  its running-app server with Claude Code, Codex or Hermes on a background
  worker, linking the skill into that agent's user skills folder unless one of
  that name is already there. Downloads open the native folder picker directly and report the result
  in a compact notice. Temporary macOS translocation paths block registration
  and skill links; independent skill copies remain available.
  Saved menu layouts that predate Help retain access to it. The transport has a
  master playrate knob, context menu, key-bindable pitch steps and
  per-project display range (ADR-0045). Its follow button cycles how the
  view follows the playhead: by page, off, or smooth scrolling while playing,
  saved per project; the wheel zooms at the playhead while following, at the
  pointer when off. Projects and renders default to
  `~/Documents/<slug>-media` (the render folder is choosable), each project
  in a folder of its own with its own media; imports are copied into the
  project by default, and Save as into another folder copies the media. FX
  chain presets, track templates (a strip and its chains) and project
  templates (with a copy of their audio) are saved, loaded and deleted in
  the app. Tracks duplicate from either right-click menu with their chains,
  items, sends and automation (`track.duplicate`), the whole selection at
  once. The menu bar's Track menu only adds strips and saves or adds track
  templates; a track's own edits are in its right-click menu (no shared
  entries). In the mixer, Ctrl dragging a plug-in onto another strip copies its
  whole chain, and a plug-in's right-click menu renames that instance.
  Dragging one of several selected tracks (TCP or mixer) moves them all.
  Keys act on the panel under the pointer (Delete or Alt+C removes items
  over the arrange view, notes over the MIDI editor). Every key that is not
  part of a gesture or a focused control is a rebindable action, and the
  actions list shows all of an action's keys. The arrange view's
  right-click menus cut, copy, paste at the pointer, duplicate, delete and
  consolidate items; the MIDI
  editor's grid menu deletes, copies, cuts, pastes and duplicates notes. Each effect's row in the FX chain
  has a presets menu: the plugin's factory presets, read-only, and the
  user's own, saved and deleted there (`fx.preset.*`, the sandbox's preset
  format, kept per plugin in the configuration folder). Takes: show all takes in lanes,
  explode onto tracks or along the track, delete one, Ctrl-drag one out, and
  render the selected items through their FX (MIDI to audio) in place, in
  place deleting the FX, or as a new take. Trims act on every selected item.
  The FX chain selects several effects (Ctrl/Shift+click) to bypass, offline
  or remove together; a double click opens an effect's editor. FX chains and
  the routing matrix use separate resizable utility windows owned by the main
  window, with per-kind positions/sizes in the project UI sidecar. Their raised
  theme surface matches the former popups, and OS titles follow their headers
  (including FX track/take switches and track renames). Their menus,
  browsers and MIDI learn dialogs stay in their window; unused keys reach the
  app keymap; dialogs opened by forwarded shortcuts activate their owner.
  Project replacement closes FX overlays and clears their pending requests.
  MIDI learn has one app-wide owner across roots, including deferred closes.
  Concurrent plugin browsers reload favourites before toggles to preserve edits.
  Empty-chain double clicks add main, input or take FX. Each root
  repaints on demand and follows theme switches. The main window
  waits on app/local plug-in/ModelLoop notifications and demand-driven
  deadlines; stopped transport and stationary meters have no refresh timer.
  Frames are capped near the display rate (at most 120 fps) even where
  vsync is off.
  Input/commands coalesce UI sidecar checks, and writer completion wakes main.
  TCP rows keep their full top line at a 24-unit minimum; older sidecars
  clamp on load. Alt+wheel anywhere over the arrange view, and Ctrl+wheel
  over the TCP, resize every track by the same step, with the row under the
  pointer anchored (editable in the mouse map); Ctrl+wheel over lanes
  scrolls. Proportional track height zoom is removed; legacy
  mouse bindings fall back to current defaults and saved absolute heights
  still load. Every wheel combination over the TCP, the arrangement's lanes
  and ruler, and the MIDI editor can be rebound in Edit > Mouse
  modifiers.
- **Rack window** (ADR-0062, gui.md §4.12): a separate window with a tab per
  rack (`+` adds one), a plain pannable, zoomable space (10-400 %, each
  rack's view in the UI sidecar), modules drawn from their faces with knobs,
  toggles, choices and jacks in signal colours, hanging cables, selection,
  marquee and group moves, knob drags, cables drawn jack to jack or onto a
  knob, modulation rings, telemetry glow, a searchable module browser
  (double-click or drag in) and context menus; View → Racks and the toolbar.
  Module displays (`draw()`), dashed feedback cables, plugin modules (a
  click opens the editor), Automate on a knob (a lane under the home
  channel), the source view with Make editable copy, rack Outs in a
  channel's input menu, Show/Add rack in its right-click menu, and pointer
  input on module displays (`pointer()`), MIDI learn from a knob's menu (a
  dot marks a bound knob), patches (rack presets saved from the space or
  a selection, and the factory patches, on the browser's own Patches view,
  apart from the modules, searchable, with a card on hover; one loaded
  into an empty rack keeps its own layout, the rack's Out moved into it,
  and is shown whole), an empty rack's "Add a module, or start from a
  patch", and cables into a plugin module's plug-in parameters (dropped
  on its body, picked from a searchable list; sockets on its edge with the
  parameter's name on hover, amounts
  dragged there; a cable's right-click sets the amount or removes it), and
  for big patches: type or double-click to add a module at the pointer (a
  cable let go on nothing picks a module that takes it), copy, cut, paste
  and duplicate with the cables among the modules, arrow-key nudges, fit to
  the selection, Tidy up (`rack.arrange`, the rack or the selection), zoom
  out to 10 % with module names drawn readable, and the
  hovered or selected modules' cables brought forward while the rest fade.
  Knob and jack names keep to cells of their own (smaller, wrapped or cut
  rather than running into a neighbour or under a control), and cables glow
  with their source's level (`moduleOutputLevel` for sources out of view,
  while the transport plays). Copy and paste go through the engine's module
  clipboard, so a pasted plug-in keeps its settings in any rack and after a
  cut. A channel fed by a rack shows a Rack device at the top of its FX
  chain window: the rack's name, Open, and its Macro modules' cabled knobs,
  named after what they drive, turned as one undo step, with a rack knob's
  Automate and MIDI learn, following automation as it plays; its mixer
  strip shows the rack as a slot above its plug-ins (click: that device;
  double-click: the rack window). A module with pitch and gate outputs
  captures its notes to a channel's MIDI item from its right-click
  (`rack.capture` as a job, 1 to 16 bars: the status bar shows its progress
  and a Stop button; a rack or channel changed meanwhile adds nothing and
  says so). The window's plug-in parameter list cables a plug-in without
  semantic keys by its CLAP id (`fx_3.12`).
- **MIDI learn** (ADR-0027): any plug-in parameter (the FX window's MIDI
  button, or `fx.learn.set` for agents) and any action (Ctrl+L in the
  actions list) learns a controller by moving it: CC, note, pitch bend or
  program change, absolute, three relative encoder modes or toggle, soft
  takeover, and "only when selected / focused / visible" for parameters.
  Parameter bindings are saved with the project and chain presets; action
  bindings in `controls.json`. Every track's fader, pan and buttons (mute,
  solo, polarity, mono, arm, monitoring, a master's lane mute) and every
  send's level, pan, mute, polarity and taps learn their own controller from
  a right-click (TCP, mixer, routing matrix; ADR-0058), and so do VCA
  faders, mutes and solos in the mixer, saved with the project
  (`track.learn.*`, `route.learn.*`, `vca.learn.*` for agents). "(MIDI CC)" actions
  drive the selected tracks' volume and pan, the master's volume, the master
  playrate, a beat jog, the last touched send and the last touched plug-in
  parameter (the volume, pan and playrate ones with optional soft takeover);
  the learn dialog warns when a knob is learned for an action that only runs
  per press.
- **Agents**: the MCP server for the running app or a headless engine, for
  any MCP client (Claude Code, Codex, Hermes, …); the bundled skill
  (`skill/`). The proxy transport deadline exceeds the maximum `job.wait`
  duration, avoiding a timeout race during long optimization jobs.
  There is no built-in agent panel (ADR-0043).
- **Analysis and sound design**: `core-analysis` (BS.1770/R128 loudness
  validated against EBU Tech 3341/3342, spectral, pitch, envelope, stereo,
  comparison, masking); the CMA-ES patch optimizer (`specs/optimizer.md`);
  the LLM evaluation harness (`tools/eval-harness`).
- **Stock plugins** (ADR-0011, ADR-0047): Marram, Coppice, Pollard, Drumlin, Yarrow, Sundew,
  Treeline, Gorse, Hickory, Sorrel, Teasel, Bracken, Ghyll, Lichen, Withy,
  Pollia, Umber, Sedge, Gain and Hornbeam, each passing clap-validator, a reset determinism check, an unload
  test and the headless REAPER check;
  JSON patches and the semantic-parameters extension.
  Yarrow, the EQ, is the first written as a DSP script
  (`plugins/yarrow`) and compiled natively from it, playing the same
  samples as the script in the player: eight matched zero-latency bands
  (bells, shelves, cuts to 48 dB/oct, notch, tilt) on stereo, mid, side or
  one channel, dynamic bells and shelves, band listening, gain scale, auto
  gain, and an editor of its own with a 32768-point spectrum line. It
  replaced Facet (`com.suppleaudio.eq`). Sundew, the compressor
  (`plugins/sundew`), is the second: six characters (Clean, Glue,
  Opto, FET, Vari-Mu and the upward Bloom), the feedback ones solved sample
  by sample; an external key, a key high-pass, link, mid/side, lookahead
  (a restart), tempo-locked release, colour, mix and loudness matching; an
  editor with a scrolling history with bars and beats, a transfer curve, a
  gain-change needle and a plot of each character's ballistics. It replaced
  Ballast (`com.suppleaudio.comp`). Treeline, the limiter
  (`plugins/treeline`), is the third: a true-peak ceiling (a 4×
  interpolating detector, within 0.01 dB to 20 kHz), attack along two
  overshoot-free curves blended by how sudden the music is, a hold that
  spans a cycle of a leading bass line, a two-stage release, 4.5 ms of
  latency at 48 kHz; BS.1770/R128 loudness of the output (momentary,
  short-term, integrated and LRA over the last 30 s or accumulated since
  play, true peak) against a target,
  level-matched and delta listening, and an editor with a history of what
  was held down, the loudness and the reduction in its sustained and peak
  parts, and plots of what the engine is doing. Gorse, the transient
  shaper (`plugins/gorse`), is the fourth: attack and sustain the
  same at any level, a detector whose hold spans a cycle of what plays (so
  held tones, bass lines and voices get no gain), a focus on the lows,
  mids or highs, a 1 ms lookahead that lands the gain on each onset, and a
  soft clip that keeps sample peaks under 0 dBFS; an editor that lays the
  recent hits over each other at their onsets and shows what each part
  changed, and a history with each hit it found. It replaced Flint
  (`com.suppleaudio.transient`). Hickory, the stereo utility
  (`plugins/hickory`), is the fifth: gain, mute, polarity, channel
  routing (swap, left, right, mono, side), width, balance, bass mono on
  the side only, a DC block and a delay of up to 50 ms to line tracks up,
  every switch crossfaded and the output the input bit for bit at rest;
  readouts of the last 3 s (peak, correlation, balance, side level) and an
  editor with an image of where the sound sits and the correlation an
  octave at a time, where bass mono's crossover is dragged. It replaced
  Plumb (`com.suppleaudio.utility`). Sorrel, the gate and expander
  (`plugins/sorrel`), is the sixth: with lookahead (a restart, 2 ms
  by default) its raised-cosine opening ends a millisecond before the
  level crosses the threshold, so onsets pass whole; it closes at a steady
  rate in dB, eased in, and with Follow at the rate the sound was already
  decaying at (within a factor of four of the release); hysteresis, hold,
  range to exact silence, Expand with a ratio, an external key with high-
  and low-pass, and key and delta listening. It keeps the key's levels of
  the last 10 s as a histogram and reads from it a noise floor, a loud
  level and a suggested threshold (the close level 2 dB above the noise's
  peaks), with the time open and the openings; its editor shows what came
  in, what goes out and what was taken, the histogram, the gain, a
  transfer plot with the hysteresis loop and a sketch of the timing. It
  replaced Sluice (`com.suppleaudio.gate`). Teasel, the saturation
  (`plugins/teasel`), is the seventh: twelve curves in three
  families (soft, tape, tube, transformer; transistor, diode, clip, fuzz;
  fold, rectify, crush, alias), four of them with memory (tape's
  hysteresis, tube's sag, a transformer core that saturates the bass
  first, a sample-and-hold), each with a character control and bias;
  driving only a band, the mid or the side; emphasis, cuts, tuned feedback
  through the curve, dynamics that drive the attacks or the sustain, and
  an LFO free or in time on the drive, character, bias or pitch;
  oversampling to 16x, minimum or linear phase, with the dry signal and
  the clean bands inside it,
  and auto gain that holds a −18 dBFS RMS signal's level at any drive.
  Its readouts give the THD and even share of a 100 Hz sine through the
  curve as set and the distortion it adds to the music; its editor shows
  the spectrum in and out with what the curve adds, the curve with where
  the music sits on it, the harmonics, the dynamics and the LFO. It
  replaced Kiln (`com.suppleaudio.drive`). Bracken, the delay
  (`plugins/bracken`), is the eighth: each side's loop in two halves,
  so every second echo can come early or late with each pair still on the
  beat (groove); stereo, ping-pong or mono; time free or in note values,
  bent like tape or faded to when it changes; tone, diffusion and a pitch
  shift in the loop; echoes forward or reversed; clean, tape or a bucket
  brigade (darker the longer the time) worn by age, with drive into the
  medium's curve; modulation, ducking, an input to throw sounds in by,
  width, and a freeze that holds the loop exactly. It tells the host its
  tail as the time, feedback and tempo change, and reads out the echo time
  and how long the echoes take to fall 60 dB; its editor shows the echoes
  to come (dragged across for the time, up and down for the feedback) with
  a playhead from the last sound, each echo's tone, the medium and the
  ducking. It replaced Echelon (`com.suppleaudio.delay`). Ghyll, the
  reverb (`plugins/ghyll`), is the ninth: a room built from its size
  in metres, the materials of its walls, floor and ceiling, and where the
  source and the listener stand. Its reflections are the room's mirror
  images off up to three surfaces, each as late, weak and dark as its path
  and heard through an ORTF or spaced pair, a head or one omni; its decay is
  each octave's Eyring reverberation time, the air's absorption counted;
  its tail is a sixteen-line network as long as the room's mean free path,
  at the reverberant level for the listener's distance; below the
  Schroeder frequency the room's modes ring at their own frequencies, as
  loud as the two places sit on them. Decay, scatter, predelay, early and
  late levels, cuts, width and a freeze. It reads out the RT60 at three
  octaves, the distance, the critical distance, the first reflection,
  clarity (C80) and definition (D50); its editor is the room to scale (the
  source, the listener and the walls dragged), a section for the height,
  the materials, every reflection on a time line (one hovered traces its
  path through the room), and each octave's decay with the modes beneath.
  It replaced Nave (`com.suppleaudio.reverb`). Lichen, the analyzer
  (`plugins/lichen`), is the tenth: it never writes its output (host-
  smoke holds it to a bit-exact pass-through) and reports as read-only
  parameters BS.1770 loudness (momentary, short-term, integrated, range,
  their highest), true and sample peak, PLR, crest, overs, clipped runs,
  DC, correlation, width, the width below 250 Hz, balance, mono loss, the
  seven named bands, tilt and centroid (as the offline analysis defines
  them) and the key with its strength, over the last 60 s of the play (or
  render), or all of it with `accumulate` (the editor's clock, as in
  Treeline). Agents read them with `fx.meters`. Its editor shows the loudness against
  a target, the last minute and the play's distribution, the spectrum now
  (mid and side), its peak and the play's average with the named bands, a
  vectorscope with correlation by third octave, the pitch classes round
  the circle of fifths, and a health list. It replaced Gauge
  (`com.suppleaudio.analyzer`).
  Marram, the synthesizer (`plugins/marram`, `docs/specs/marram.md`),
  replaced Orrery (`com.suppleaudio.orrery`, removed); the synth guide
  (`skill/guides/sound-design.md`), the evaluation tasks, the optimizer's
  tests, the MCP demos and the B1 recipe use it. Four
  oscillators (analog with sync, wavetables from Orrery's recipes, FM
  operators, noise, sub; FM between any two), unison up to 8 copies, two
  filters (ladder, state-variable, comb, formant; serial or parallel),
  amp and mod envelopes with fixed amounts (cutoff, pitch, FM depth),
  velocity and key tracking, drift, glide, Poly/Mono/Legato with a
  polyphony limit, oversampled 1x/2x/4x; modulation routes from a third
  envelope, three LFOs (per note or free, synced or not, seven shapes),
  four macros and the MIDI sources to some sixty per-voice destinations;
  and fifteen factory sounds. Its parameters say when they apply, so an
  agent sees only the oscillators and filters a patch uses. Its editor
  draws each oscillator's wave (the wavetables' own frames), the filters'
  response with a node to drag, the envelopes and LFOs with handles and
  the newest note's place, the voices playing and the
  route list. It passes the stock checks and the REAPER check.
  Withy, the chorus (`plugins/withy`), is a stock plugin with no
  predecessor. Five styles lay out its voices: classic (two in anti-phase,
  left and right), dimension (two cross-mixed: the widest, with the
  shallowest combs), ensemble (three a third of a cycle apart), multi (two
  to eight at their own delays, rates and places) and vibrato (one voice, no
  dry). The depth is the pitch each voice swings in cents, the same at any
  rate, free or in note values locked to the song; sine or rounded
  triangle, a fast shimmer, a slow random drift, and bloom, which holds the
  voices still at each new note and lets them sway again (a singer's late
  vibrato). Voices read through a 16-tap windowed sinc, flat within
  0.01 dB to 16 kHz however fast they sweep; clean, or a bucket brigade
  whose band follows its clock, with its lean and compander hiss. Low keep
  leaves the lows dry and centred; high cut, feedback with its level held,
  width and an equal-power mix. It reads out the detune, the sweep, the
  rate and the mono loss; its editor weaves each voice's delay over the
  last 3 s, shows each voice's pitch now, where the voices sit around the
  listener, and the response now in stereo and folded to mono.
  Pollia, the phaser (`plugins/pollia`), has no predecessor
  either: 1 to 24 trapezoidal all-pass stages, stacked or spread over
  up to six octaves, with feedback either way (its added level mostly
  taken back); a sweep by an LFO free or in time (six shapes, a phase, a
  stereo offset) or by the playing's envelope, measured against its own
  recent peak so it needs no threshold, either direct or through a
  photocell (a lamp the cell follows, quick up and slow down); and rise
  and fall, a barber pole that turns the wet's phase through a 90-degree
  pair so the notches climb or sink forever at full depth. Keep below
  leaves the lows whole; mix runs from dry through the deepest notches to
  a vibrato. It reads out the stages' centre and the envelope now. Its
  editor draws the response with its notches, where the notches went
  over about a cycle and a half of the motion, and the path a photocell
  takes the sweep on; dragging the response moves the centre (across)
  and the range (up and down). Twelve factory patches.
  Umber, the tape machine (`plugins/umber`), has no predecessor: a tape
  machine modelled part by part and fitted to published data for current
  studio tape (third harmonic about 0.1% at reference level, 1% at +9 and
  3% at about +13 dB at 15 ips; the highs' saturation output at 10 and
  16 kHz; noise). The drive is calibrated as a machine is (0 VU,
  reference level, is −18 dBFS RMS). Class-A record and playback
  amplifiers add second harmonic. The oxide's magnetization is a tanh of
  the field (single-domain needles) squared off by the tape's squareness,
  its irreversible part pinned and relaxing toward that curve by an exact
  exponential step, so bias decides how much hysteresis is left. The highs
  saturate on their own: above a split (5 kHz at 15 ips) they meet a
  ceiling of their own, so a loud cymbal no longer squashes the mids with
  it, and the split sums back to an allpass. The tape is a continuous
  headroom (5 to 13 dB to its 3% point), four speeds, NAB or IEC (AES at
  30 ips). The playback head adds its bump (frequency adjustable), dip
  and ripple, azimuth (highs lost, the tracks apart in time), the HF trims
  of both sides; the tape adds dropouts, print-through and crosstalk; the
  transport adds wow (reels, a wander) and flutter (capstan, idler,
  friction) with each part's depth drifting, and scrape, kilohertz jitter;
  hiss with the electronics' low rumble under it, modulation noise and hum. Oversampling is 1x to 16x as the user
  sets it (best 176 to 384 kHz inside); full latency (8 ms, linear phase,
  deep wow) or low (9 samples: the wow scaled to fit, its shape kept).
  It reads out the VU level, the headroom to the tape's 3% limit and how
  much the peaks were rounded. Its editor shows the machine (reels
  turning at the tape's speed, heads, VU meters, a peak lamp), a
  magnetization scope (the field recorded against the flux played back,
  each axis scaled to the trail, drawn from 8 kHz points so the highs
  draw stars; dragged for drive and bias), the response quiet and hot,
  and the transport's displacement. Thirteen factory patches, Mastering
  among them for the subtle rounding of a mix's peaks.
  Sedge, the chromatic tuner (`plugins/sedge`), has no predecessor and is
  kept simple: one detector (McLeod's normalised square difference over
  the last `window`, 10 to 200 ms, found at about 6 kHz and refined at the
  full rate on the fundamental alone, so a string's sharp overtones
  neither bias it nor make it sag), a reading every 15 ms that counts only
  above the threshold, when clearly pitched and while the note still sounds
  at the window's end, the display holding otherwise, a 50 ms smoothing on one note and a new note shown once two
  readings agree. Equal temperament or just, Pythagorean, quarter-comma
  meantone, Werckmeister III or Vallotti on a key, A4 at 415 to 466 Hz.
  It reads out the note, octave, cents and frequency. Its editor is cells,
  as a tuner's lights: a coarse row across ±50 cents with a wide middle
  cell for the tolerance, a tuft of fine cells a cent apart above it, and
  a mark under the row at the exact pitch. Measured only on synthetic
  plucked strings so far; the user's recordings of real ones are still to
  check it against.
  Gain (`plugins/gain`), the simplest, is one knob: a level change of
  −32 to +32 dB, glided by the framework's 20 ms smoothing in dB, mono or
  stereo, no latency, and at 0 dB the input bit for bit. Its editor is
  the knob and its value. It is the one stock plugin not named after
  the natural world (ADR-0047).
  Hornbeam, the guitar amp and cabinet (`plugins/hornbeam`), has no
  predecessor and uses no impulse responses or recorded data: every part
  is a model. Five voices of two to four triode stages (Child's law, grid
  current holding the grid and charging the coupling capacitor, the
  cathode's bypass, the bright capacitor on the gain pot), the passive
  three-knob tone stack solved from its circuit, presence, a push-pull pair
  under a sagging supply and an output transformer, all oversampled but
  the transformer; its level holds as the gain turns. The speaker is a
  motor and a cone of five masses on springs solved into modes (the
  resonance in its box, the breakup above a kilohertz), driven through the
  amp's output impedance, its suspension stiffening and its motor weakening
  with excursion; the mic hears each ring of the cone by its own paths
  (the cap sits behind the rim) with its pattern and proximity, then the
  box's other speakers, open back or edge, the floor and, for the ribbon,
  the wall behind it. Six cabinets, four speakers, three mics placed
  anywhere on the cone 1 cm to 1 m away; the amp alone (into a speaker's
  load) or the cabinet alone (driven clean). The editor draws the
  cabinet's response from the same model as the audio (within 0.001 dB of
  it) and the mic where it is. Mono by default (the left input through one
  amp, as a pedal's L/MONO jack): 13.7 refs at the defaults against
  Teasel's 15.7; stereo, an amp per side, costs 25.0. Sixteen factory patches, cleans to high gain, bass and
  the cabinet or the amp alone. Tuned by measurement on a synthetic DI;
  nobody has listened to it on real guitars yet.
  Coppice, the sampler (`plugins/coppice`, `docs/specs/coppice.md`), has
  no predecessor. It plays one sound, a user's file or one of seven
  factory samples rendered from Marram (`tools/factory-samples`, CC0,
  0.9 MB inside the plugin as `factory:<name>`): classic (pitched from a
  root, with a forward loop crossfaded or a ping-pong loop, snapped to zero
  crossings, sustaining while the key is held or playing on), one-shot, or
  slice (at transients or on a grid, a slice a key, trigger, gate or thru);
  start, end, reverse, normalize; one ladder or state-variable filter, two
  envelopes, two LFOs, four macros and Marram's routes (velocity to the
  start, among others); 32 voices oversampled as Marram's. Its editor draws
  the waveform with flags, the loop and its crossfade, or the slices and
  their keys, every voice's place, close-ups of the start and the loop's
  seam, and the slice keys or the root over its keyboard. Twelve factory
  patches. 0.18 % of a core at the defaults. Not yet: zones and velocity
  layers, time-stretching, a file's root or `smpl` loop points, and free
  zoom of the waveform. The script player has no factory files: a script
  that defaults to one plays nothing there until a file is chosen.
  Pollard, the drum sampler (`plugins/pollard`, `docs/specs/pollard.md`),
  has no predecessor. Sixteen pads on the GM keys from C2, each a file or
  one of twenty-four factory hits rendered from Marram (CC0, 0.6 MB), played
  by Coppice's engine (the two scripts share it as identical text, checked
  by `plugin.pollard.engine-copy`): a region, forward or reversed,
  one-shot or gated, an envelope, a state-variable filter, level, pan;
  eight choke groups; variation per hit (pitch, level, start), the same in
  every render; the main output and eight more, which make Steppe's track
  18 channels. Its editor plays a pad where it is pressed (an instrument's
  `pointer()` may call `playNote`). Seven factory kits. 0.48 % of a core at the defaults. Not yet:
  velocity layers and round robin across several files, and sends or
  effects per pad.
  Drumlin, the drum machine (`plugins/drumlin`, `docs/specs/drumlin.md`),
  has no predecessor. Sixteen pads, each on a MIDI note of its own
  (`padN.key`, General MIDI's drum map by default, so agents and the
  notation's drum names find each drum), synthesizing thirty-two realistic
  drum sounds without samples: the classic kit's thirteen and the studio
  kit's nineteen. Each is a recipe of modes and noise bands made a sample
  at a time at a DAC clock, through that DAC with unsmoothed steps (the
  classic kit's 8-bit companded, the studio kit's linear 16-bit), and for
  the classic kit the kick, tom and conga filter and the hi-hat's VCA over
  one looping recording. The recipes were
  fitted to banks of real recordings by `plugins/drumlin/fit` (CMA-ES over
  the script compiled natively); the recordings are not in the repository.
  Tune, Decay (the recording's own at 100 %), choke groups, variation per
  hit (Vary, 50 % by default), eight extra outputs, nine kits; the editor
  picks a pad's sound by kit. 0.31 % of a core at the defaults. Not yet:
  more kits, and a sound of the user's own fitting.

- **DSP scripting** (ADR-0032): plugins written in a checked subset of
  C++, run by a JIT player plugin, exported as plugins or C++ projects. The
  compiler and runtime exist (`plugins/suite`, tested against the same scripts
  compiled as C++). So does the player (`plugins/suite/player`, built from
  `plugins/template`, staged with the stock plugins): one plugin per script
  in the script library, with the semantic-params extension, reloading a
  script when its file changes. `dsp.write`, `dsp.edit` (text replaced in
  place), `dsp.check`, `dsp.get` and `dsp.list` write and compile scripts (the app's library is
  `<data>/<slug>/scripts`, shared with the sandbox), `dsp.test` measures
  one with standard signals or an audio file at any sample rate (and, when
  asked, parameter automation, a timed sidechain key or a note sequence)
  and reports its meters as of each signal's end and what its
  `trace(name, value)` calls recorded over each signal
  (`docs/specs/trace-extension.md`); `dsp.preview` plays a file too; a
  saved test or preview uses a bounded, filesystem-safe ID component with a
  stable hash when changed or ending in the reserved hash suffix, so raw IDs
  cannot impersonate encoded names; result metadata retains the full ID. A
  script tells the host a tail that
  follows its settings and the tempo (`setTail`), and a script that compiles is loaded
  and set like a stock plugin (`skill/guides/dsp-workflow.md`).
  `fx.describe` and the sandbox's `plugin.describe` report a live
  instance's loop-budget stops, out-of-range indices and failed reloads as
  structured problems. Scripts declare read-only factory patches
  (`factoryPatches(Patches&)`), listed and applied with `fx.patch.list` and
  `fx.patch.set {name}` or the sandbox's `preset.list/load {source:
  "factory"}`, and offered by exports' preset bar and hosts' preset
  browsers. A parameter may apply only while a toggle, a choice or a number's
  range declared above allows it (`.activeWhen()`, `.activeUnless()`,
  `.activeAbove()`, `.activeBelow()`): the schema marks it
  `"active": false` and says when it applies, descriptions leave it out,
  `plugin.describe`, `patch.merge`, `fx.patch.merge` and `fx.param.set`
  name it under `inactive`, and the editor, its own or the automatic one,
  dims its control. Scripts declare modulation sources (`Source` members,
  per voice or global; instruments also get velocity, key, random,
  pressure, slide and five MIDI controllers), and the patch's `routes`
  member connects them to `.modulated()` and `.polyModulated()`
  parameters with amounts in the parameter's unit (a frequency's in
  octaves), through a second source when given; the schema lists the
  sources and each parameter's amount unit, errors name the right unit,
  state and presets keep the routes, and factory patches set them. An
  instrument's `setVoiceLimit()` plays fewer of its voices. The editor
  shows the routes as a list to edit (the automatic editor's, or
  `e.routes()` in a script's own), written through the patch's checks.
  `dsp.export` makes a script a CLAP plugin for any host (a copy
  of the player with the script appended, which offers that one plugin), or
  a C++ project of the plugin template that builds the same plugin, bit for
  bit, with the script compiled as C++ (every example's export is checked
  against the player, for semantic conformance and factory patches, and
  passes its own tests). Effects and instruments
  (voices the runtime manages, CLAP note expressions and per-note
  modulation; no note effects); samples and impulse responses
  as files a patch names (loaded whole, no streaming from disk); spectral
  effects (an STFT), oversampling and sidechains. A script may draw its
  own editor (`editor()` for controls, `draw()` on an HTML-canvas-style
  `Canvas`, `pointer()` to drag and click what it draws, editing
  parameters as a user would, `Shared` members for what the audio code
  shows it); one that does not gets the automatic editor. `dsp.preview`
  draws a plugin's editor to a PNG without a visible window (EGL on Linux,
  Metal on macOS, WGL on Windows; the editor-picture extension), after clicks, drags and wheel turns it is
  given, and says what they edited. Plugin-file exports run on Linux,
  macOS and Windows.

Benchmarks are in `docs/benchmarks/`: B1, 75 Marram tracks with EQ and
compression at 48 kHz and 64 frames without xruns using anticipation, 28 to 29
without (`b1.md`); the comparison with REAPER, which sustains 82 to 83
(`b2.md`); GUI cold start and frame costs (`gui.md`); Console 9
strip cost (`console9-strips.md`). The benchmark harness also supports 64/128-frame requests, installed third-party effects,
whole-callback/render-job tails and an observed ring low-water mark. Timing is
bench-only; synthetic deadline counts and server xruns are separate. Historical
capacity reports retain their original revision/settings.

## Known limits

Library boundaries:
- The control client bounds connecting and sending, as well as the
  response, by its timeout through nonblocking sockets; that path is tested
  on Linux, and native macOS/Windows CI has not run on it.

Engine and hosting:
- Modular racks (ADR-0062/0064): Racks do not follow a
  track's playrate map;
  cables into a plugin module's parameters modulate CLAP plug-ins only
  (VST3 has no modulation), by their first voice, at most 16 per plug-in;
  parallel paths summed inside a patch remain unaligned (ADR-0064);
  post-fader sends into an In module take the linear path.
- The headless platform layer (ADR-0035) has natively tested macOS/Windows
  address waits, wake sources, control socket/MCP paths and user directory roots.
  Both native engines run with dummy/offline audio and native RT/offline
  equivalence coverage. Windows file helpers and audio-file I/O pass native CI.
  Native hosting/scanning (ADR-0036) and PortAudio device builds (ADR-0038) are
  verified; native bridge targets are enabled (ADR-0037), with native host
  editor paths supplied by ADR-0040. Hardware behavior still needs opt-in device trials.
  Windows event waits admit 64 unique handles. Generic macOS High QoS must be
  self-applied. Native device helpers instead use a startup barrier with Mach
  time constraints (macOS) or MMCSS Pro Audio (Windows). On macOS the live
  helpers also join the opened device's CoreAudio workgroup (engine.status
  `workgroup`, ADR-0038); the renderer and background pool and the out-of-process
  bridge do not (the bridge would need a Mach-port channel it lacks). Hardware
  trials of the join remain opt-in and unmeasured. Linux process memory-lock and CPU
  latency preferences report unsupported on other platforms. Windows MCP is
  intended for piped/file input; cancelling an interactive console read can
  await a keypress during shutdown.
- Only level and pan edits skip compiling (ADR-0060). Mute, solo, routing and
  plug-in changes compile the session, and with playrate scheduling started
  each also prepares a rate plan, which costs several milliseconds per edit in
  a session of a few dozen tracks.
- A touch-mode write hold ends 500 ms after the last move, not when the
  control is let go (ADR-0061): nothing reports a strip control's release to
  the engine, so a fader held still longer than that hears its old curve.
- REAPER's default anticipative FX sustains about 10 % more tracks than
  Steppe with anticipation on B1's session (82 to 83 against 75, B2). The
  anticipative renderer is the limit: it ran at 78 % of real time at 64
  tracks while the callback stayed under 5 %.
- Live multiprocessing requires at least two usable CPUs and successful
  helper scheduling; otherwise the engine reports a warning and runs serially.
  A serial plug-in chain cannot benefit, and a stalled helper that has claimed
  a job stalls its cycle. Wake/scheduling overhead remains on light graphs;
  there is no adaptive cost threshold.
- The shared DSP guard implements x86 FTZ/DAZ and arm64 FPCR FZ; macOS arm64 CI
  verifies FPCR restoration and subnormal handling. Plug-in lifecycle calls run
  in the thread's own floating-point environment.
- On Linux, real-time background threads need `RLIMIT_RTPRIO` ≥ 1, an unlimited
  `RLIMIT_RTTIME` and a pool that leaves two allowed CPUs free; without them
  the renderer and pool run at normal priority (with a warning) and compete
  with desktop load.
- A bridged plug-in's audio thread that cannot take the engine's background
  policy stays at normal priority with no warning or status: nothing reports
  it to the host. The bridge inherits the host's limits, so this needs a
  limit or policy change made to the bridge process from outside; such a
  thread's plug-in can then fall behind under load like an unelevated
  renderer.
- There is no pipelined bridge mode, so a bridged plugin on a live
  (monitored) track is passed through with a warning; set its isolation to
  in-process to hear it while monitoring.
- Audio backends: Linux JACK, dummy, and native PortAudio (CoreAudio on macOS;
  WASAPI/ASIO on Windows), with selection, latency/xruns and restart notifications.
  Fake-device output equals offline in both executors, with native CI coverage.
  The engine writes stereo; additional opened device outputs are silenced by
  both native and JACK backends.
  Hardware latency, disconnect behavior and scheduling benefit are unmeasured.
  Edit > Audio device (ADR-0050) chooses and saves the audio system, devices,
  sample rate and buffer size, restarting a running engine; the GUI starts the
  native device by default on macOS/Windows. On Linux a rate is suggested to
  PipeWire, or the rate and quantum are forced on the graph while the engine
  runs. Whether a given PipeWire server grants a suggested rate, and how
  CoreAudio, WASAPI and ASIO devices take each rate and buffer, are untested
  on hardware. The dialog lists the devices present when it opens; there is no
  hotplug refresh while it stays open.
  Refusing native MIDI inputs are skipped with structured startup warnings;
  audio and other inputs keep running. CoreMIDI/WinMM use bounded per-port queues,
  timestamp mapping, bounded per-port merging, routing/learn/recording and
  non-restart topology advisories
  (ADR-0042). Device-free timing/overflow/lifecycle tests cover the shared path;
  physical MIDI timing/disconnect behavior needs SUPPLE_MIDI_DEVICE_TEST=1 trials.
  SysEx is discarded; WinMM timestamps have millisecond resolution. WinMM names
  do not distinguish two identical devices, or a replacement from the original;
  this will be addressed with Windows MIDI Services once its App SDK is released
  for production (ADR-0042).
  No direct PipeWire/ALSA backend or MIDI outputs.
- MIDI learn has no OSC, no "arm envelope" option, and no learn on the
  routing matrix's channels menu. The last
  touched parameter misses edits in plug-in editors that report no gestures
  or outputs. Every MIDI input is heard as a control;
  there is no per-device "control only" setting.
- Plugins that change state without notifying the host (LSP, for one) have
  that state captured only when the project is saved: no undo step and no
  dirty mark.
- Stopped-transport audio from tracks that are not live (a plugin's own
  preview button, for example) is not heard in anticipative mode unless the
  track is armed and monitored.
- A plugin's factory presets are the semantic extension's factory patches:
  a third-party plugin's own presets (CLAP preset discovery, VST3 program
  lists) are not listed in the presets menu, and the menu does not mark the
  preset last loaded.
- Multi-output plugins: main output ports wider than stereo are not used,
  and there are no per-output meters.
- A master's hardware output carries its main pair only: auxiliary channels
  (3+) of a wide master cannot reach the device, and an output has no gain
  of its own. A key route from one output master into another master's path
  leaves the first one early at the device (a plan warning gives the
  samples). The metronome always plays on outputs 1/2. Native CI has not yet
  run the multi-output engine on macOS or Windows (CoreAudio and WASAPI
  opening every output of a device).
- Overlapping gestures on a plugin without semantic parameters share one
  state blob.
- The render dialog's Start and End show beats and bar.beat rather than
  the ruler's full format.
- MP3 encodes run one at a time (LAME's shared tables, `libs/audio-codecs`):
  several MP3 renders in parallel jobs wait for each other's encoding step.
- Renders: MP3 and Ogg Vorbis files are byte-identical from run to run on
  one build; different platforms or compilers may round LAME's and
  libvorbis's floating point differently, so they are not compared across
  platforms. AIFF and Ogg Vorbis are render outputs only: neither imports
  as media. RF64 WAVs (beyond 4 GiB) carry no metadata chunks. The LAME,
  libvorbis and libogg builds (our own config headers, no autotools) have
  run on Linux only; macOS and Windows native CI is still to run them.
- `fx.meters` reads a plugin's readouts live or as the last `render.mix`
  ended; other renders (stems, regions, A/B) keep none.
- FX containers are not implemented; parallel FX run in series on pinned
  stages and strips with auxiliary channels, and oversampling is not
  available to plug-ins with auxiliary outputs.
- A bridged plug-in's tail is read once when a sandbox render starts, so a
  tail it changes during that render is not followed (in-process plug-ins'
  are).
- VST3 host text conversion uses bounded, per-call Unicode conversion instead
  of the SDK's shared converter. Unpaired UTF-16 surrogates become U+FFFD;
  malformed UTF-8 parameter input is refused. Native verification is pending.

Session and editing:
- Consolidation's shared engine/model code has local Linux coverage; native
  macOS/Windows validation remains pending before merge.
- Project files accept `record.mode: output`, but output capture is not
  implemented: starting recording with an armed output-mode track fails with
  `record.output_unsupported` before creating files. Switch that track to input
  mode to record; existing project files still load.
- Engine stop/device restart retains finished audio and MIDI takes and commits
  them before app teardown. Headless exit does not save the project; clients
  must stop and save first to preserve MIDI and other session edits. Audio
  WAVs/sidecars remain recoverable; MIDI has no crash-recovery file.
- Save As into a subdirectory of the source media folder skips its destination
  subtree, including resolved symlink aliases, so copied files are not copied again.
- Master playrate 0.25..4x and its tr_0 envelope persist in schema 2.1.
  Tape and pitch-preserving source conversion run before take FX/live mixing;
  MIDI, automation, plugin tempo, loops and transport share the fractional clock
  (ADR-0045). The transport knob, menu and the master Playrate lane edit it;
  knob ranges are UI state and never clamp saved values. Seeded stretch phase
  variation uses a portable integer RNG, so it matches across standard
  libraries. Non-unity audio/MIDI
  recording is refused, including queued transitions. Writing the playrate
  holds nothing over its curve (strip levels and pans do, ADR-0061). Compatible scalar-only live edits publish bounded clock
  controls, retain source/converter state and glide over the time between
  moves through a 150 ms low-pass (one smooth pitch glide for a mouse drag or
  MIDI knob, at the cost of some extra lag) without graph
  compilation or an epoch change (ADR-0046). Structural prediction retains
  pending scalar ramps. A knob glides through 1.0 too; leaving 1.0 from the
  unity bypass glides the same way. Mode/curve edits wait for compensated lookahead
  and coalesce during prepared transitions; long-latency take chains can exceed
  the 200 ms horizon.
  All source allocation/priming runs on the control thread; the renderer and
  callback adopt prepared state at scheduled boundaries under RTSan. Prepared
  boundaries and lateness checks use the renderer frontier, block granularity
  and observed preparation time. A live knob move becomes audible after about
  350–400 ms (the render horizon plus clock and source lookahead). Seeks prepare
  off the audio executors while playback/monitoring continues. Clock windows
  reserve four additional seconds for latency changes; history exhaustion or late
  preparation recovers automatically through a prepared locate at the consumed
  position, with the same state reset/preroll as a seek. Moving a compensated
  stretch source uses this recovery too, preserving phase alignment. Under
  read-ahead PDC, inserting track latency can move sibling sources and trigger
  this same recovery, even when the clock window itself has enough history.
  Render-command budgets conservatively use the minimum curve value; a long
  varying-rate range may need splitting even when its exact duration would fit
  the output budget.
- Track groups and optional nested VCA controls are implemented (ADR-0052).
  Effective VCA gain is capped at Console 9’s native top (about +12.0412 dB);
  queries expose the requested and applied levels. Edit links require matching
  item boundaries and lanes; MIDI/take-content/warp edits stay local. Native
  macOS/Windows validation is pending.
- Audio warp (ADR-0053) v1 supports aligned bundles, pitch-preserving Signalsmith
  takes, nonlooped items and linear slopes only. Slice mode requires unwarped
  input; arbitrary-span bundles, curved slopes and musical beat-domain markers
  are deferred. Invalid legacy maps remain stored but play at scalar rate with
  warnings. Very large stretch ratios can smear attacks. Slice gaps fade to
  silence; there is no synthesized tail filling.
- Source edits prepare immutable audio on a background worker; initial engine
  startup still waits synchronously for its first sources. Decode is not cancellable
  mid-call, and the cache remains memory-backed. Native CI for audio warp and
  background preparation is pending on macOS/Windows.
- Loop recording passes that span a change of the loop range are not
  handled; MIDI recordings have no crash recovery.
- Master recording (ADR-0057): a plan change that moves a master's latency
  while it records shifts the rest of the take, as output-latency changes do
  for channel takes. Starting a recording while already playing lets an
  earlier print be heard (never recorded) for up to the render-ahead span.
  Masters wider than two channels record their main pair. Projects with an
  armed master or items on a master do not open in builds before schema 2.5.
  Linux is tested locally; native macOS/Windows CI has not run on it yet.
- `fx.copy` does not copy envelopes or key routes; take FX cannot be keyed.
- The history budget's size estimate walks shared payloads per version, and
  squashed steps keep large invocation arguments.

GUI:
- Automation-item point editing requires an amplitude magnitude above 0.000000001; restore
  the amplitude before editing a flattened item.
- Plan compilation (including media decoding/resampling), Lua execution and
  some project/file operations still run synchronously on the model/UI thread.
  The 2 ms responsiveness target is not a bound on these operations; Lua's
  instruction budget excludes time spent inside commands.
- The opt-in native popup-focus regression passes on Hyprland/XWayland,
  including typing and Escape after a forwarded floating-window shortcut.
  Opening a popup in an already-key owner skips raising it, preserving Cocoa
  bridge-editor ordering; a background popup also skips activation before any
  SDL raise. Explicit panel reopen still raises the panel. Native focus waits
  are bounded and report missing focus events instead of hanging.
  Cocoa bridge stacking and Windows activation remain pending native CI/manual checks.
- FX/routing utility windows have not been checked by hand on Hyprland,
  Cocoa or Windows: WM floating, owner stacking and XWayland positioning
  remain manual checks. Native CI for these changes is pending coordinator
  validation; local Linux tests do not establish macOS/Windows behaviour.
- Help onboarding tests pass native CI on Linux, macOS and Windows. Real
  harness registration, App Translocation and the native folder pickers have
  not been tried by hand.
- Native shell/toolkit builds and tests pass CI (ADR-0039).
  Cocoa/Windows hidden-window tests replace Linux EGL offscreen;
  Linux retains pixel reference tests. Native editor embedding is integrated
  (ADR-0040), with combined native builds and hermetic tests passing CI.
  Windows config keys use a protected current-user ACL; filesystems without
  ACL support fail the save. macOS cold-start timing reads the kernel process
  start time. GUI socket/screenset and source-export integration fixtures are
  enabled and pass native CI on macOS and Windows. macOS bulk rectangle edges
  align to framebuffer pixels so partial repaints match complete repaints exactly.
  Script-player headless pictures (`dsp.preview`) use framework 1.24.0's
  EGL, Metal and WGL renderers. Native CI requires pictures and pointer edits;
  missing devices/drivers return `dsp.no_picture` with a corrective hint.
- No autoscroll during drags; no resize cursor; plugins can be dropped onto
  mixer strips but not onto track-panel rows.

Plugins:
- Bracken, Ghyll, Pollia, Umber and Withy discard non-finite inputs before their recursive
  state. Their mono/stereo regressions and the script runtime's scalar/aggregate bounds
  and prepared-tail lifecycle fixes pass focused Linux dev/RTSan checks.
  Marram's free-running LFOs use whole-frame phase anchors so their routed
  modulation remains bit-identical across host block sizes, including rate changes.
  Native macOS/Windows CI remains pending for these changes and the player's
  Unicode library filename discovery fix.
- Marram's playing-transport LFO sync uses whole-frame phase anchors and
  ignores host beat rounding within 1e-9 beats. Seeks, loop jumps, division
  changes and transport restarts re-anchor; tempo-only changes keep phase
  continuous. Wobble Bass passes exact 128/511-frame partition and native/JIT
  comparisons, including transport changes and sample-offset division automation.
- Script numeric literals use locale-independent parsing, including float
  rounding and underflow, so comma-decimal host locales accept the same source.
- What a script's `prepare()` sizes (Buffers, DelayLines, Stfts,
  Oversamplers) may hold 1 GiB per instance together; a request past it is
  refused before allocating and the script stops with
  `script.runtime.memory`, and a hot reload that asks for more keeps the
  previous version. Its reset copy holds as much again.
- DSP source exports require a visible, automatable parameter with a plain
  name for their host tests and do not retain the JIT's loop budget (a loop
  that never ends hangs a native export). Exports include declared factory
  patches, but do not package external audio files or the patch of a
  currently loaded instance. Preview requires an editor build and an
  available offscreen renderer (EGL, Metal or WGL with framebuffer objects).
- Yarrow has no linear-phase mode; its editor (drawn by the script) takes
  no keyboard input or right-click menus, draws the summed curve without
  regard to each band's channel, and analyses the output only. Its
  analysis costs about 0.3 ms per transform in a standalone -O2 copy (at
  most 25 a second at 48 kHz, under 1% of a core, on the editor thread);
  the plugin itself has not been profiled.
- Lichen's readouts refresh every 100 ms and reach a host as meter events
  at 30 Hz, so a render's last reading can miss its final 100 ms or more.
- clang-cl builds the suite runtime and stock scripts with
  `/clang:-ffp-contract=off` and `/clang:-fwrapv` (they had neither); native
  Windows CI has not run since.
- Scripts need a JIT: there is no fallback for hosts that forbid executable
  memory (MIR's interpreter is a test backend, not real-time safe).
- A script save that redefines the plugin while an editor drag is under way
  can leave that gesture open in the host until it rescans.
- Stock CLAP builds and product/conformance/reset checks run on Linux, macOS
  and Windows. Existing macOS AUv2 targets compile in CI but are not host-tested
  here. Stock VST3 builds remain opt-in; native VST3 hosting is tested with an
  in-tree SDK fixture. The pinned clap-validator checks every stock plugin
  and the player on all three platforms in CI; it stays optional locally
  (`tools/validator/build.sh`), and its tests skip without it. On macOS,
  `mac-embedding` (template 1.31.3) opens each stock editor in a Cocoa host
  window, as `x11-embedding` and `win32-embedding` do on Linux and Windows.

Smaller recorded items: `describeFlat` skips hidden parameters in full
detail; core-params formats magnitudes beyond six decimals imprecisely;
`Rng::uniform` at extreme endpoints; forced-schedule tests for `RtHandoff`.

## Planned

- Profile the 64-frame worst case; a pipelined bridge mode.
- Close the capacity gap with REAPER (B2) by making the anticipative
  renderer cheaper, then keep refining capacity in both modes.
- MIDI outputs, a disk-backed media cache.
- Lua ergonomics (a script library path, hot reload).
- Native audio/MIDI hardware validation and GUI hardening on macOS and Windows.
