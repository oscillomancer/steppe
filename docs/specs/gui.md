# The DAW GUI

This spec describes the GUI as built and is updated with it. Decisions it
builds on:
ADR-0005 (SDL3 windows, GL3 + NanoVG, own retained toolkit, performance
gates), ADR-0039 (native windowing, scaling and keys), ADR-0007 (command bus), ADR-0008 (versions, branches, undo),
ADR-0019 (typed strips), ADR-0030 (main-thread notifications/deadlines),
`ARCHITECTURE.md` §3 (threads, event delivery) and §10, and the workflow research in `docs/research/reaper-ux.md` (the top-40
list in §16 is the priority order for workflow coverage).

Goal: a complete track can be produced in Steppe without falling back to
another DAW for core tasks. Performance gates (ADR-0005 §5): p95 frame
≤ 8 ms CPU at 1080p with a 64-track session in the arrange view at any zoom;
idle CPU ≈ 0; cold start to an interactive window < 300 ms; no UI work can
stall audio.

## 1. Process and threads

The GUI application is the engine process with a window: one binary
(`<slug>`, from `cmake/Product.cmake`; the headless engine stays
`<slug>-engine`). The UI thread **is** the model thread
(`ARCHITECTURE.md` §3): it runs the command bus, owns the session, services
CLAP main-thread callbacks and hosts plugin GUIs. Consequences:

- **Every change is a command.** Views dispatch typed commands with
  `Origin::Gui` through the same dispatcher as MCP and Lua.
  The GUI has no private mutation path; a gesture that needs many edits is a
  `batch` (one undo step). Continuous gestures (fader drags, item drags) use
  the bus's coalescing (one version per gesture, intermediate previews as
  coalesced edits — see §4.3).
- **The UI reads, never waits.** It renders from the current immutable
  session version (structural sharing makes holding a version cheap) and
  from telemetry rings (meters, playhead, scopes). Nothing in the UI calls
  into the audio thread or blocks on workers.
- **Long work leaves the thread.** Peak building, waveform decoding, plugin
  scanning, renders, analysis, file dialogs' I/O and Lua scripts run on
  workers and commit results as commands or post completion events. Budget:
  no synchronous task on the UI thread over 2 ms (`ARCHITECTURE.md` §3),
  measured (§9). As built: the browser's Rescan runs `plugin.scan
  {wait: false}` (every module in the supervised scanner, the catalogue
  installed when its job is done); the UI sidecar's coalesced writes go to a
  writer thread (exit and project switches write synchronously, after it);
  waveform lookups of media without a content hash read the file's size and
  mtime at most every two seconds and again on each new session version.
- **Event-driven loop.** `SDL_WaitEventTimeout` with the timeout set by the
  nearest deadline: app service (`engine.md` §6), local CLAP timers (also
  without editors), ModelLoop timers, pending notation compilation (300 ms),
  dirty render previews (40 ms by default), and armed UI sidecar checks.
  One parked `FdWatcher` borrows native waitables and watches the app relay, local plug-in descriptors and
  ModelLoop's wake source; readiness posts an SDL wake, dispatches on main, and
  stays out of the wait set until consumed/rearmed. Posted worker results also wake SDL.
  Signal shutdown wakes through ModelLoop's pipe even without any deadline.
  The 100 ms service and 10 ms control polls are removed: MIDI batches use the
  relay. Remote instances use only `setMainThreadWake` forwarding; the GUI
  does not poll a remote editor or its bridge descriptors. Playback frames
  run at 16 ms; transport counters refresh at 250 ms only while playing or
  recording, with stopped state/xrun/error changes refreshed on notifications.
  Visible moving/decaying meters have a 33 ms frame deadline, including their
  1.5 s peak hold; at rest the feed arms/rechecks one audio activity wake at
  its -60 dB display floor. Clip latches alone do not animate. Idle with no
  work or deadlines blocks indefinitely. Every window swaps with vsync and
  presents frames at least `ui::minFrameInterval` apart: three quarters of
  the frame period, where the frame rate is the display's refresh rate
  divided by the smallest whole number that brings it to 120 Hz or below
  (60 fps at 60 Hz, 72 at 144 Hz, 120 at 240 Hz; 60 Hz when SDL reports no
  rate). A frame held back by it is a wait deadline, so the cap bounds drags
  and animation where vsync is off without adding a poll. The main window of
  a benchmark run (`--frames`) and snapshots are not capped. A window whose swap interval
  cannot be set logs `no vsync`. FD service failure is reported and
  ends the app loop rather than adding a fallback poll. Offscreen idle
  measurements use the same service/watch/deadline path with a finite end bound.

### 1.1 Observing the model

Views observe the session through the bus's event stream (per-topic
sequence numbers; `event.dropped` → resnapshot, `ARCHITECTURE.md` §3). Each
view keeps a **view model** keyed by stable object ids (`tr_12`, `it_7`…)
and keys its snapshot cache by (project generation, version). Project
replacement forces a full resnapshot, even when ids and versions repeat.
Within a generation, a version change diffs the objects it shows (the model's
`diff(versionA, versionB)` already exists for ADR-0008) and invalidates only
the affected nodes. A view never caches pixels across versions without a
key that includes the object's content revision. The arrange view uses the
version store's change list for adjacent edits and undo within the same
generation; branch jumps compare
shared object identities. Track geometry changes rebuild the rows; other
edits update affected controls, item indexes and old/new item footprints.
Unrelated FX parameter edits preserve TCP control/capture identity and do
not rebuild or repaint the arrange view. Envelope glyphs stay within their
lane or take's item.

### 1.2 UI state is not session state

Zoom, scroll, selection, window layout, open editors, FX/routing window
placements, plug-in editor placements, docking and
screensets live in a separate per-project UI file (`<name>.ui.json`,
canonical JSON, never in the project's diff) plus per-user settings
(keymaps, toolbars, theme) under the user config directory
(`<paths::config()>/<slug>/`). Selection is exposed read-only to agents
(`gui.selection` query) because "the selected items" is useful context;
agents never depend on it for correctness.

Before project replacement, the old project's sidecar is flushed. The new
project's sections reset to their registered defaults before loading its
sidecar; absent files or sections retain those defaults. Sidecars use
`supple.ui/1.0`; a file without a `schema` reads as 1.x, and a different
schema major is treated as absent and its file is left untouched. Save As preserves the current layout and copies it to
the new sidecar, subject to the same schema protection.
Filesystem lookup or read errors likewise leave the sidecar untouched and use
the default layout on project replacement, without throwing from the GUI loop.
User-setting loaders (keymaps, mouse maps, menus, preferences, MIDI controls and
plug-in favourites) return file errors for failed lookups; only genuinely absent
files select defaults.

Any input event (including editor geometry) or command processed on main arms
one compare/write check about a second later. Continued input coalesces into
that earliest check, so long gestures still persist; unchanged idle UI has no
periodic check. Internal wake events do not arm writes. Writer completion/failure
posts to main; failures arm a bounded one-second retry. Project switches and
exit retain explicit flushes.

The `windows` section stores one placement for each floating panel kind:
`{"fx": {"x": ..., "y": ..., "width": ..., "height": ...}, "routing": {...}}`.
Desktop positions use SDL screen coordinates; sizes use design units. Geometry
is retained on close and restored on reopen, regardless of the FX track/take.
Missing sections preserve first-open defaults. Invalid fields are ignored,
sizes clamp to the panel minimum and 8192 units, and a position with no visible
48-unit corner on a current display is discarded. Open panels adopt the new
project's placement on project replacement. Screensets continue to store the
mixer/history docks and arrange layout; floating panels are not screenset entries.

Screenset names trim leading and trailing spaces and tabs. Import skips
empty names and silently keeps the first entry when trimmed names collide.

## 2. Toolkit (`daw/gui/ui`)

A small retained-mode toolkit, ours (ADR-0005 §4). Not a general framework:
only what the DAW needs, designed for virtualized, data-heavy views.

- **Nodes.** A tree of `Node`s (id, role, bounds, children, flags). Roles
  (`button`, `slider`, `list`, `row`, `track`, `item`, `text-field`…) are
  semantic: they drive keyboard behaviour, UI tests and, later, AT-SPI
  accessibility. Custom-drawn views (arrange, piano roll) are one
  node each with their own hit testing and internal virtualization.
- **Layout.** Row/column stacks with fixed / fraction / content sizing, gaps,
  padding, min/max; absolute placement inside timeline-like views. Layout is
  pure (inputs → rectangles), cached per node, recomputed only when size or
  content revision changes. All layout in *design units*; the window's
  display scale (SDL) maps them to pixels, so fractional scaling is a single
  factor.
- **Input.** Hit testing front-to-back, pointer capture during drags,
  hover, wheel with precision (trackpad deltas), modifiers, double-click.
  Keyboard: focus chain, Tab navigation, shortcuts resolved by the actions
  system (§6) with context sections (arrange, MIDI editor, mixer, text
  field). Key releases reach the focused node and its parents
  (`Node::onKeyUp`), for keys held as a gesture's modifier (Space-drag
  pans the rack window); key presses say whether they repeat. Text input through SDL3 text-input events (IME composition shown
  inline). Replacing multiline editor text preserves the caret's line and
  code-point column (clamped to the new text); diagnostic columns remain byte
  offsets. Hiding a dock or panel releases focus and pointer capture within
  its subtree (`Root::releaseWithin`), sending `Cancel` to the captured
  control. Continuous edits end as one undo step; uncommitted drag previews
  and pending button clicks are discarded. Focus and capture outside the
  subtree are preserved. A press outside the topmost popup closes every
  popup above the one it lands in (all of them when it lands in none) and
  reaches nothing else, so a dialog's menu button, or any other spot in the
  dialog, closes the open menu rather than opening a second one. A closing
  popup restores focus only to a node
  still in the tree, visible through all ancestors and enabled through all
  ancestors; otherwise it restores no focus.
  Normalized round knobs share slider gesture/default, wheel and keyboard
  behavior. Their relative vertical drag spans 150 design units, with Shift
  for fine adjustment; double-click resets to the configured default. Model
  readback is ignored while a knob is being dragged. Every drag reports
  Begin/Move/End and each one-shot change reports Set.
  Faders, pan controls, sliders and knobs coalesce their drags (ADR-0060):
  while one holds the pointer capture, window motion events are kept, the
  latest only, and reach it once per frame, so a drag's edits go out at the
  frame rate rather than the pointer's event rate (125–1000 a second). Any
  other input, such as the release, first delivers the kept motion; hiding the
  control mid-drag drops it. The main loop delivers it as input just before a
  frame, then services the model. `Root::pointer` (tests, scripts) delivers
  every motion at once, as does a capture that does not coalesce (item and
  envelope drags, drawing tools).
- **Damage.** Each window keeps a dirty-rect list; nodes invalidate
  themselves; a frame redraws only damaged regions (scissored). Removed nodes
  survive the current input dispatch; focus, hover and capture are resolved
  again after callbacks. Views own overlay registrations and release them
  when destroyed. Animated
  nodes (meters, playhead) are separate *layers* with their own small dirty
  rects so a running transport does not repaint the arrange view.
- **Rendering.** Per window one GL 3.3 core context (macOS: 4.1 forward compatible).
  - **Chrome** (widgets, text, icons, outlines) through NanoVG GL3.
  - **Bulk layers** in plain GL with persistent buffers: waveforms (§3.1),
    MIDI notes as instanced rectangles, envelopes as line strips, meters
    and spectra as small textures/buffers. Bulk data never goes through
    NanoVG (the spike measured 81 ms/frame doing so).
    Rectangle borders use framebuffer pixel coordinates so partial and full
    scissors produce identical border coverage.
  - Order per frame: background → bulk layers (scissored per lane) →
    chrome → overlays (drag ghosts, menus, tooltips).
- **Text.** NanoVG fonts: Inter (UI) and IBM Plex Mono (numbers, notation,
  code), OFL-1.1, bundled (`assets/fonts/`, recorded in `THIRD_PARTY.md`).
  Tabular figures for every numeric readout.
- **Theme.** A token set (colours, type scale, spacing, radii, stroke widths,
  motion durations, strip-type colours) loaded from JSON; light and dark
  variants; widgets read tokens only, never literals. Extends the plugin
  template's `Theme.h` contract so stock plugin editors and the DAW share
  tokens. Colour values are `#` followed by exactly six hexadecimal digits.
  Sizes (`type`, `space`, `radius`) are finite, non-negative numbers within
  the nonzero range of `float`, or zero. Invalid values produce structured
  errors with escaped JSON-pointer paths. Theme and arrange hex colours share
  the same strict colour parser.
- **Animation.** Explicit, short, interruptible (eased, ≤ 150 ms) and only
  on state changes the user caused; animation requests frames, idle stops
  them.
- **Testability.** The toolkit renders into an offscreen framebuffer
  (SDL's offscreen video driver or EGL surfaceless) for golden-image tests
  (per-pixel tolerance, stored as PNG under `daw/gui/tests/golden/`), and a
  test API drives nodes by id/role (click, type, drag) and reads back state.
  GL readback, not XGetImage (ADR-0005 open item). Non-presented frames compose
  content and overlays into a framebuffer at the observed pixel size; resized
  offscreen EGL drawables need not resize their original back buffer. Readback
  binds the source root's context and capture target explicitly.

## 3. Data-dense rendering

### 3.1 Waveforms

A per-media **peak pyramid**: min/max (and RMS) per channel at block sizes
64, 256, 1024, 4096, 16384 samples, built by an I/O worker on import or
first display, cached on disk under the user cache directory keyed by media
content hash (never in the project, `reaper-ux.md` §5). The arrange view
picks the coarsest level with ≥ 1 bucket per pixel column, uploads one
vertex pair per column per visible item into a persistent buffer, and draws
the visible span; scroll shifts the buffer window, zoom rebuilds only
visible items. While a pyramid builds, the item draws a placeholder and a
progress hint; editing never waits for peaks. Take rate/stretch maps
item-time to source-time before sampling the pyramid.

### 3.2 MIDI

Notes as instanced rectangles (position, length, pitch, velocity colour,
selection) from the item's sequence (the same compiled representation the
engine plays), in the arrange view (mini view) and the piano roll; CC lanes
as line strips/bars. Loop thumbnails include source-crossing notes held from
the preceding cycle, clip them at the item's boundaries, and omit source
note starts at or beyond the loop length, matching playback. Offsets and
play rates apply to these tails too; loop-pass and note-glyph drawing remain
bounded to 8192 each. Without a positive source length, the thumbnail uses
one nonlooping pass, as playback does.

### 3.3 Meters and telemetry

Meters read the engine's lossy telemetry rings (peak, RMS, and for masters
momentary/short-term LUFS from core-analysis) at 30 Hz, with peak hold and
clip latches as view state. The playhead reads the transport telemetry. No
meter ever forces a full repaint.

## 4. Views

Each view lists the commands it needs. A missing command is added to the
engine (with tests and generated docs) before the GUI uses it; the GUI
never works around the bus.

### 4.1 Main window

Transport bar (go to start, play/stop, record, loop, punch, metronome,
count-in and follow as symbol buttons: |◀, ▶ or ■ while playing, a red ● (a
ring while counting in), circling arrows, [●], a metronome, →| with the bar
count, and the follow mode of §4.3 as a frame (the view) and a line (the
playhead): the line inside with ▸ by page, outside the frame when off, inside
with motion lines when smooth; each is lit in its colour while on — green
playing, red recording, accent for loop, punch, metronome, count-in and a
follow mode other than the default (off or smooth; by page it is unlit) — and
keeps its state word as its value text ("follow page",
"follow off", "follow smooth"); position in bars|beats and
time, tempo/meter, CPU/xrun/anticipation readouts from engine telemetry; the
rate and buffer readout shows the device's period, e.g. the JACK quantum);
toolbar (actions, §6: only what the transport bar does not already have —
undo, redo, add channel, the actions list by default); the arrange area (track control panel + timeline);
bottom docks for the mixer and MIDI editor, and a history dock; separate
floating windows for the FX chain, routing matrix and plugin GUIs. Render and
other dialogs remain popups. Window layout is UI state (§1.2).

FX and routing are resizable utility windows owned by the main window, using
the same SDL utility/parent mechanism as plugin editors (ADR-0005/0039).
Each has its own GL context, bundled fonts, retained root, focus, damage cache
and popup stack. Resources are created on first open and retained while
hidden. Both panels fill their client area with the theme's `raised` surface,
matching the former popup frame. Their OS titles match their visible headers:
"Routing", "FX: <track>", or the take FX header; switching tracks/takes or
renaming the track updates the FX title in place. Close or an unhandled Escape
hides only that panel; Escape first closes a nested popup or cancels a widget
gesture. Events are routed by SDL window identity before the main window's
quit handling. Unused keys reach the app keymap with the floating root's typing
context. Opening a popup activates its owning visible OS window only when
another window of this app has keyboard focus, including main-window dialogs
opened by shortcuts forwarded from FX/routing. Background popups do not request
foreground activation; hidden snapshot/test windows stay hidden. An already-key owner is not raised, keeping
normal-level Cocoa bridge editors above it when opening a menu. Explicitly
reopening a floating panel still raises that panel, including when it is key
or the app is in the background; that path follows an explicit user action.
X11 supplements the activation request with
an input-focus handoff only when SDL and the X server agree that one of this
app's windows already owns focus; Cocoa/Windows use SDL's native activation.
Theme/palette switches repaint all roots. Each visible root paints only on
damage/animation, with its context forcibly rebound after plugin editor calls; hidden roots do not
paint. `--snapshot out.png` also writes `out.fx.png` and `out.routing.png`
for open floating panels, using their own GL readback.

The transport tempo button opens a BPM field (1..1000) with Tap, Set and
Cancel. Tap estimates BPM from the mean interval between the latest eight
taps, starting with the second tap, and fills the field with at most one
decimal place. A gap longer than two seconds starts a new sequence, leaving
the field unchanged until the second new tap. Estimates are clamped to the
field's range. The toolkit has no input timestamps, so taps use a steady
clock at button activation. Tap returns focus to the field; Set or Enter
applies the value through `tempo.set`, preserving the other tempo points
and ramps. Cancel discards it; reopening starts a fresh tap sequence.

The transport's time-map controls appear as meter, tempo, a small playrate
knob and a two-line Rate readout (ADR-0045). Unity is at the knob's top/center
in all display ranges: logarithmic travel on either side maps 0.25..4.0
(default), 0.5..2.0, 0.75..1.5 or 0.9..1.1 to its endpoints. Double-click
resets to 1.0. Drag moves use `transport.playrate.set` with one origin gesture
per drag, hence one undo step; model/effective readback cannot take over
while dragging. With an active nonempty master playrate envelope, readback
uses `EngineApp::playrateState().effectiveRate` from consumption telemetry;
otherwise it shows that accessor's `requestedRate`. It does not display the
accessor's distinct `envelopeValue`, which describes the latest authored curve
at the consumed position and may differ from the audible rate. The engine owns
the cached curve; the transport keeps no local envelope cache. Readback executes
no commands and adds nothing to the command log.

Right-click either the knob or readout for reset, semitone up/down
(multiply/divide by `2^(1/12)`), ten cents up/down (`2^(1/120)`), Apply playrate
to current BPM, the preserve-pitch toggle, and checked display ranges (the
first is labeled `Playrate range: 0.25-4.0 (default)`).
Pitch steps are bounded to the model's 0.25..4.0 range. Reset, the four pitch
steps and preserve toggle also appear as key-bindable transport actions;
the menu runs those same actions. Tempo bake uses
`transport.playrate.apply-to-tempo`; command errors reach the normal status
message. Display range is per-project UI state in section `transport`, key
`playrate_range` (strings `0.25-4.0`, `0.5-2.0`, `0.75-1.5`, `0.9-1.1`);
unknown/missing choices use the default. Changing range never edits/clamps
the saved or automated rate: knob geometry saturates while the readout keeps
the actual value.
The action **Transport: playrate (MIDI CC)** (`control.transport.playrate`)
lets a MIDI knob or fader drive the rate along the same travel, in the chosen
display range; CC 64 of 0..127 (a centre detent) gives exactly 1.0. An encoder
moves the rate ten cents a step. Like the on-screen knob it runs
`transport.playrate.set`, one undo step per movement.

### 4.2 Track control panel (TCP)

Per strip: name (rename in place: a double-click; Enter or a click anywhere
else keeps the edit, Escape drops it), **strip-type badge** (Channel / Buss /
Master: colour, shape and label differ — clear at a glance, ADR-0019),
mute/solo (rounded pills in their own colours), arm (a red disc: faded off,
solid armed), monitor (a speaker: an outline off, solid on, green auto),
input (channels only for these three), polarity ("Ø") and mono pills at the
start of the level row (ADR-0022), volume fader and pan
(Console 9 Channel's controls on channels and busses; plain gain and
balance on masters; the fader's dB scale comes from `libs/console9`'s
mapping), "fx" (the chain window) with "+" beside it (the plugin browser
alone, adding last in the chain; no count, to save room), the "out" button
(a click opens the routing matrix; drag onto another strip to create a
route; drop rules follow `route.strip_type`, invalid targets show why before
the drop; a strip the track already sends to is not a target, since the
matrix shows one send per pair), meters. The mixer's strips carry the same controls in the same
looks (§4.4). The button row starts at the TCP's left edge with mute and
solo (over polarity and mono) and ends at its right edge with "env" and
"out", at any depth; a channel's input button takes the room between.
A master that is armed or holds items has a row too, its **lane** (ADR-0057):
the header shows only the master badge, its name, the lane mute ("M", a pill
in the mute colour, `track.set {lane_mute}`: it silences the master's own
items, never the master) and the arm; the master's fader, pan and FX stay in
the mixer. A lane-muted master's items are drawn dimmed. A master with
neither has no row.
Rows shrink to 24 design units: the name, strip badge, "fx" and "+" fit
above a four-unit bottom resize grip. The lower buttons hide below 40 units;
the fader/pan row hides below 58. Track heights clamp to 24–400 units,
including compact folders and older UI sidecars; expanded takes retain that
minimum per take lane, plus their item name bar. MIDI key heights remain
independent (4–48 units, §4.7).
Every view names a track the same way (`model::trackLabel`):
its name, which track.add sets to the first free `ch_N`, `buss_N` or
`mstr_N` (the session's own master is "Master"); an unnamed track from an
older session shows that kind of label too.

A strip control used on a selected track acts on the whole selection, in the
TCP and the mixer alike (`gui::SelectionEdit`): mute, solo, polarity, mono,
arm, monitoring and the input set the same value on each (monitoring and input
only on its channels, arm on its channels and masters); a fader or pan drag moves each selected track by
the same amount from where it started (clamped). A click is one undo step, a drag is
one. A control on a track outside the selection acts on that track alone.
Folder/buss parentage shows as indentation and a parent bar. Under each
parent the channels are listed first, then the busses (each in the parent's own
child order, `views::displayChildren`), so a buss never sits between two
channels; the model's child order is not changed (the mixer groups by type
too, §4.4). Dragging an empty part of a row (not a
button) reorders the track among its siblings *of the same type* under the same
parent: an insertion marker shows the drop, Escape cancels, and the release is
one `track.move {track, index}` (`views::reorderIndex` computes the index;
no-op drops commit nothing). A buss cannot be dropped between channels, and
a track does not change parent by dragging. The panel's foot ("mixer",
"+ strip") is a band the rows never cover; the tracks scroll far enough for
the last one to clear it. Adding a strip
offers exactly **Channel, Buss or Master** (`track.add {type}`); a
double-click on the panel's empty space (or the mixer's, §4.4) adds a channel
and selects it; a right-click there opens the same Channel / Buss / Master menu
at the pointer. Track selection is shared by the TCP and the mixer: a click on
a row's or strip's empty part selects it alone, Ctrl+click toggles, Shift+click
extends from the last click in that view's order; a right-click on a selected
track keeps the selection, and so does a plain press on one, so a drag from
it moves every selected track of its parent and type together, in their
order, as one undo step (`views::reorderMoves`, track.move steps); released
without a drag, it selects that track alone. The track menu's Delete (and the Delete key, when
tracks were selected after items) removes every selected track as one undo
step (`track.remove:selected`; the main master stays; children before folders).
Its Duplicate copies every selected channel
and buss right after its original as one undo step (`track.duplicate`, see
session-model.md) and selects the copies. The menu bar's Track menu shares
none of these entries: it adds a channel, buss or master, adds from a
template and saves one (loading an older `menus.json` drops the track edits
it used to list: Color…, Channels…, add plugin, Duplicate and Delete
selected). A right-click on a row's fader,
pan, mute, lane mute, solo, polarity, mono, arm or monitoring button opens
that control's menu instead, "MIDI learn: Volume…" (naming the bound control,
"(CC 7 · ch 1)"): the learn dialog of §4.5 for that track alone, binding
through `track.learn.set` (ADR-0058); the mixer's strips do the same.
Commands: `track.*`, `route.*`, `fx.*` (bypass/offline), transport.

### 4.3 Arrange view

Ruler (bars/beats and time; tempo and meter lanes; markers and regions),
virtualized track rows (only visible rows exist), items with waveforms or
MIDI, take lanes, fades and crossfades, automation lanes (volume/pan/mute
on strips; plugin parameters), time selection, snapping grid, playhead with
follow modes. By page (the default), the view jumps when the playhead leaves
it, putting the playhead a quarter of the way in; off, the view stays where
it is; smooth (`continuous` in the UI file), while playing the playhead walks
to the middle of the view and the view then scrolls under it in whole device
pixels (so waveforms, ticks and text move without shimmering), a playhead out
of view is centred at once, and while stopped the view pages. Zoomed out
until bars cannot all carry a label, the ruler labels every 2nd, 4th, 8th…
bar counted from bar 1, the stride set by the zoom and the narrowest bar in
view, never by where the view starts. The
transport bar's follow button cycles page, off and smooth
(`ui.follow.cycle`); `ui.follow.page`, `ui.follow.off` and
`ui.follow.continuous` set one, for keys. The mode also sets where the wheel
zooms (below): at the playhead while the view follows it, at the pointer when
off. The mode is UI state, saved per project (§1.2). The ruler's bars row draws the loop range (the time selection)
and its time row the punch range; a new punch range turns punch on, and an
edge dragged onto the other edge removes the range (punch or loop off). A
right-click in those rows offers Loop on/off, Punch on/off, Punch range =
loop range, Loop range = punch range, Delete loop, Delete punch and Delete
loop and punch. On every version change, deleted track and item IDs are
pruned from transient selection; undo restores objects, not old selection.
MIDI editor selection clears on external version changes, and a gesture
cannot commit against a different project generation or version from the
one where it started. An automation point drag (on an envelope or an
automation item) is dropped, with an `env.changed` message, when the points
changed elsewhere between the press and the release. Automation-item point,
move and resize drags also compare the press-time amplitude, baseline, position,
length, source length, loop state, envelope and pool. A change cancels the drag
with `env.changed`; it does not rebase the draft onto the changed item.

Gestures (P0, `reaper-ux.md` 8–13): drop files onto tracks/timeline, move,
trim edges, split (S), duplicate, slip (Alt-drag), stretch (with the rate
and pitch-preservation mode visible), fade/crossfade handles, take switch.
Each gesture previews locally (a ghost) and commits **one** command or batch
on release, so a drag is one undo step and never floods versions;
continuous controls (faders) send coalesced `*.set` edits during the drag
and one final value (the bus's gesture coalescing). A pooled TCP row pins
the original track for fader, pan, resize and rename gestures; rebinding
cancels the gesture. Wheel zoom holds the **project cursor** (the playhead,
the transport position the position display shows) still while the view
follows the playhead (by page or smooth), or the mouse pointer when following
is off; a cursor outside the visible
lanes falls back to the pointer. Keyboard zoom (`ui.arrange.zoom_in/out`:
= or Shift+= or Keypad +, - or Keypad -) anchors at the playhead. The
timeline's start stops the view, so zooming out near it pulls the anchor left
on screen; zooming back in returns it to where it was, as each step holds
the anchor where the first step of the run found it. The run ends when anything
else moves the view in time (a scroll, follow, the scrollbar), the cursor
moves, or (with the pointer anchor) the pointer moves. Horizontal zoom is continuous;
vertical track height per track and global.
By default Alt+wheel anywhere over the arrange view, and Ctrl+wheel over the
TCP, adds eight design units per notch to every track row, including hidden tracks and compact folders, preserving
mixed heights and clamping each independently. Fractional wheel deltas work
continuously; expanded rows divide the step among their take lanes. The row
under the pointer stays anchored at the same fractional position unless a
scroll boundary prevents it. Heights are UI state, saved with individual
resizes, without commands or undo entries. Over the lanes Ctrl+wheel
scrolls vertically. Over the ruler there is no row to anchor; scrolling is
clamped normally. Proportional track-height zoom is
removed. Saved absolute `row_height` values still load. All of these are
defaults of two mouse contexts (§6): `tcp.wheel` over the TCP (plain scrolls
the tracks, Ctrl and Alt resize them) and `arrange.wheel` over the lanes and
ruler (plain zooms, Shift scrolls in time, Ctrl scrolls the tracks, Alt
resizes them; other combinations do what the plain wheel does). Either can
bind any combination to scroll the tracks, scroll in time, zoom in time or
resize every track. Where the binding zooms, a sideways wheel scrolls in time;
a zoom over the TCP holds the lanes' left edge when it anchors at the pointer.
A trim moves the same edge of every selected item (and the pressed one) by
the same beats, snapped at the pressed item and clamped so none passes the
timeline's start or the minimum length; locked items stay. Alt on the right
edge stretches the audio items and trims the MIDI ones.
Left trim shifts every take's source offset by the trimmed duration times
its play rate, in the same transaction; dragging the left edge out past a
take's material (down to the timeline's start) and slipping its material
later both leave silence first (a negative offset), audio and MIDI alike. Clipboard track offsets are in
channel order; pasting skips envelope rows and reports items dropped for
lack of a target channel. Project replacement clears the item clipboard.
MIDI thumbnails map offset, rate and loop passes
as playback does. Fade shapes and curve bends share playback's evaluator.
Automation drawing samples only visible time and bounds loop glyph work;
plugin automation interpolates in the playback parameter domain (ADR-0010).

A strip's envelope menu lists, after its own and its effects' envelopes, its
sends' volume, pan and mute (`env.add {route}`, flat at the send's value;
titled "Send → destination · Volume"). A send's visible lanes sit under the
strip it leaves, after the strip's own and its effects' lanes; `route.set`
records into them in touch, latch and write like `track.set`.

The main master's (`tr_0`) envelope menu also offers Playrate (ADR-0045);
other strips, including secondary masters, do not. Creating it starts flat
at the saved project rate. The rate menu's `Master envelopes...` entry and
the key-bindable `Master: envelopes` action open this menu even when the
master strip has no arrange row. Visible master automation lanes appear
above the master's child tracks, after the master's own lane when it has one
(armed or holding items, ADR-0057); the children keep their depth either way.
Items on a master's lane are edited as on a channel's (move, trim, split,
fades, gain, copy and paste); only audio items land there: a drag, paste or
take-out of a MIDI item does not offer a master's lane, and a paste counts such
items as dropped. Audio files dropped on it become items of the master. Playrate
uses the existing lane, visibility, point
editing, shapes and automation-item controls. Its scale is logarithmic over
0.25..4.0 with a 1.0 reference at mid-height, ratio text such as `1.000x`, and
its own clipboard unit. Empty lanes use the saved scalar as their fallback
and redraw when that scalar changes. The lane display range is independent
of the transport knob's per-project display range. In the staged engine the
curve is editable and saved, but audio application remains pending.

**Menus.** Items have two menus that share no entries. A right-click on an
item opens the item menu (`ItemGestures::itemMenu`): **Cut**, **Copy**,
**Paste** (at the pointer's beat on the grid and its channel), **Duplicate**
(after the selection), **Delete**, **Consolidate items**, **Heal splits**,
**Split item at cursor** (S), **Split item at prior zero crossing**, **Mix to
new track**, then **Edit MIDI** (the item in the MIDI editor) and **Show or
hide the MIDI editor**, then the renders below (for the take under the
pointer). The menu bar's **Item** menu (`ItemGestures::itemBarMenu`, the
chrome's menu source for Item) has the rest, over the selection: **Export…**,
the timebases, the audio warp entries, the take FX and envelope entries and
the take entries below, for the heard take of the last clicked item if it is
selected (else the first selected one); with nothing selected its rows are
greyed and the take entries absent. Rows that do not apply are greyed, not
hidden, apart from the take entries. Several MIDI items are combined with
Consolidate items (engine.md). A right-click on empty lane space opens a menu
with Paste. The keys each action has are shown beside it.
Consolidate items and Heal splits sit beside each other and use the engine's
shared read-only selection planners for enablement. Consolidate captures active
audio with item/take processing and take FX into new media, while track processing
stays live; MIDI and audio partition separately per track. Its background job
reports through the existing status line and publishes all results as one undo
step. Heal is a pure edit for compatible consecutive audio pieces, including the
narrow unchanged Slice-at-hits overlap case; it keeps the surviving items selected.
Both entries are disabled for invalid/incomplete/locked/frozen selections. A lone
MIDI item consolidates only when it has several takes, all MIDI: they layer into
one take. There is no separate merge entry.
`ui.item.consolidate` and `ui.item.heal` are key-bindable actions. Engine details
and exact heal/FX/envelope eligibility are in engine.md; no new dialog or setting.
Both split entries use the project/edit cursor, independent of the menu position,
and skip selected items whose interior does not contain it. The ordinary entry
and S share the same one-transaction selection edit. The zero-crossing entry
skips non-audio active takes and is disabled when none of the selection is audio.
It searches the prepared active-take source up to 100 ms backward, respecting
source offset, rate/pitch, warp and looping. After channel mode it uses the stereo
channel with greater window energy (left on ties), before gain, fades or FX;
silence keeps the cursor, and no crossing falls back to the cursor. Pending
preparation produces a retryable status error. Linked items use the initiating
item's resolved time. See engine.md for the command contract. The toolkit's
`menuItems` accessor exposes borrowed, read-only menu rows for inspection.
**Export…** (Item menu) acts on the last clicked item if selected (else the first
selected one) and is greyed
when its heard take is empty. It opens the system's save dialog (SDL3,
asynchronous, guarded like Save As against a project change) filtered to WAV or
MIDI, starting in the project's folder (unsaved: the media home) with the take's
name (else the item ID) plus `.wav` or `.mid`, then runs `item.export`; the status
line reports "Exported <file>" or the error. `ui.item.export` is the key-bindable
action (the first selected item). See engine.md for what the file holds.

**Takes.** An item with several takes shows each in a lane under its name
bar when the row is tall enough (at least 14 units a lane), the heard one
lit and each lane named; a click on a lane hears that take, a swipe comps it
in, and the badge cycles takes. The menu bar's Item menu adds, for
several takes: **Show all takes** (a check; UI state, saved in the project's
UI file as `expanded_takes`): the item's row grows to the name bar plus one
lane per take, each as tall as the row's own height, so every take can be
seen, heard (click) and edited; **Explode takes onto new tracks** and **…in
sequence on this track** (item.explode-takes, recording order; this ends
the takes, Show all takes keeps them); **Delete take "name"** for the
heard take (take.remove); and **Keep only the
selected take**: every selected item with several takes keeps only its heard
take (take.remove of the others, one undo step). The right-click menu's
renders act on every selected item with material that is not locked, audio
and MIDI alike, one item.apply-fx each in one undo step (through the take and
track FX; on the clicked item the take under the pointer is heard first):
**Render with FX, replacing the audio** (mode replace), **Render with FX,
deleting FX** (mode replace, then fx.remove of every effect in the rendered
items' tracks' main chains, after all the renders; the replaced takes' FX go
with them) and **Render with FX as a new take** (mode take). **Ctrl-drag** below the name
bar of an item with several takes takes that take (the lane under the
pointer, else the heard one) out into an item of its own, anywhere on any
channel (take.extract, one undo step; elsewhere Ctrl-drag still copies).

**The marquee.** A drag from empty space, or a right-drag anywhere in the
lanes (on an automation lane: a right-drag, or a Ctrl-drag from empty
space), draws one marquee that selects every item it touches and every
automation point inside it, on any track and in any lane, take envelopes
drawn on items included; with the add modifier (`lane.drag`) it adds to
what is selected. After a marquee that took items and points, Delete
removes both (one undo step) and Ctrl+C, Ctrl+X and Ctrl+V take the items;
after one that took points only, the keys take the points.

**Automation lanes.** A click on a point selects it (Ctrl+click adds or
removes one) and a click on empty space adds a point there, selected.
Dragging a selected point moves every selected point, in every lane, by the
same time and the same share of its lane's height (one `env.points.set` per
envelope, one undo step); Shift frees it from the grid and Alt+click
deletes a point. A click selects in one lane; the selection names points
by position and value, so points an edit elsewhere removed simply leave it;
a press elsewhere in the lanes clears it. While a lane was clicked last,
Delete, Ctrl+C, Ctrl+X and Ctrl+V take the selected points instead of
items; points copied from one lane paste at the playhead into the lane
clicked last, points from several lanes back into their own lanes, keeping
their offsets in time. A right-click
without a drag opens the lane's menu: **Insert point**; **Select all
points**, **Copy**/**Cut**/**Paste points** (paste at the pointer, replacing
the points over the pasted span), **Delete points**, **Point shape…** (a
second menu: linear, square, slow start/end, fast start, fast end, bezier);
**Insert automation item**, or for the item under the pointer **Duplicate**
(pooled, or a copy), **Unpool**, **Loop** and **Delete**; and **Bypass**,
**Hide** or **Remove envelope** (`env.set active/visible`, `env.remove`).
Points pasted into an envelope of another kind (volume into pan) keep their
height on the lane. An automation item's header moves it; dragging its right
edge stretches its shape with it (its points and the span they repeat over,
for every item of its pool), and Alt-drag extends or shortens it by
repeating the shape instead. Point edits inside an automation item are refused
when its amplitude has magnitude at most `1e-9`; the message asks the user to
restore the amplitude first. Its source points remain unchanged.

Commands: `item.*`, `take.*`, `fade.*` (as named in session-model §7),
`envelope.*`, `marker.*`, `region.*`, `tempo.*`, `transport.*`.

### 4.4 Mixer

Strips in track order grouped by type: channels, busses, masters (several
masters are allowed, ADR-0019), each with the type's visual language; FX
slots, sends (level/pan/tap, with "follow" pan shown), input/arm/monitor on
channels, fader + pan + meters, Console 9 on/off as a project toggle with its
level-matched behaviour explained in its tooltip. Meters update only when
their entire ancestor chain is visible, and clear peak hold on reopening.

Small "fx" and "+" buttons sit at each strip's top right (the TCP's: the
chain window, and the plugin browser alone); a double-click on the name
renames the strip in place, as in the TCP. Under the chain (and its sends),
the arm, centred (channels and masters: a master records its output,
ADR-0057); an input button (channels) and "out" (a click
opens the routing matrix, a drag onto a buss or master makes a send; on a
master it reads "out 1/2", "out 5" or "out —" and opens the hardware outputs
menu instead, ADR-0056: None, the device's stereo pairs 1/2, 3/4…, then each
output alone as mono, with the port names JACK reports; the current choice is
checked and a pick runs `track.set {hw_output}`. While the engine is stopped
the menu offers None and says the engine lists the outputs); then one row with the small polarity pill ("Ø"), monitoring
(channels) and the mono pill (ADR-0022), above the pan. Everything under the
chain has a fixed height and the chain takes what a strip's missing rows
leave, so every strip's fader sits at the same height. The strip's plug-in chain lists every plug-in,
scrolling when it is longer than its section, and takes the room a strip's
unused rows would leave (sends show only when there are some, two rows of
them, scrolled by the wheel or a bar when there are more; busses have no arm
row, masters no monitoring). A send's text brightens under the pointer; a
click on it opens the routing matrix, as "out" does, and Alt+click or Delete
in its right-click menu removes it (`route.remove`, one undo step). The wheel
over a fader or pan moves it (Shift finer) and elsewhere scrolls the strips.
Alt+click on a plug-in removes it
(`fx.remove`, one undo step); a double-click on the chain's empty part opens
the plugin browser for that strip. A double-click on the strips' empty space
(not on a strip) adds a channel; a right-click there offers Channel, Buss or
Master, as the TCP's "+ strip". A right-click on a strip's fader, pan, mute,
solo, arm, polarity, mono or monitoring button offers its MIDI learn, as in
the TCP (§4.2); elsewhere on the strip, the track menu. A send dragged from "out" draws a wire from the
button and outlines the strip it would feed, as in the TCP. The
panel's top edge (shared with the MIDI editor: it lights up under the pointer)
resizes the mixer by drag and only the chain
sections follow (meter, fader, buttons keep their size); a double-click on it
restores the default, which fits five plug-ins. The height is clamped like the
MIDI editor's (what the center column leaves less a strip of the shell) and
saved in the UI sidecar's `mixer` section. Dragging a plug-in within its chain
reorders it (`fx.move`); dropping it on any part of another strip copies it, with
its live settings, last in that strip's chain (`fx.copy`); with Ctrl held the
drop copies the plug-in's whole chain, in order (the chip reads "chain (N)").
A right-click on a plug-in opens its own menu: Rename… names that instance
over its slot (`fx.set label`; an empty name goes back to the plug-in's);
Duplicate copies it, with its live settings, right after itself (`fx.copy`);
Delete takes it out, as Alt+click does (`fx.remove`). Dragging a strip's
empty part moves it among the strips of its own type only (`track.move`, an
index among its parent's children; `views::reorderIndex` in `track_layout.h` computes it, shared
with the TCP). A drag shows an insertion bar or a target outline; Escape or a
drop outside the strips cancels; each is one undo step.

A strip reorder ignores destination strips removed from the model before the
next mixer sync; stale displayed rows cannot become drop targets.

### 4.5 FX chain and plugins

The single FX window switches track/take in place and raises when opened
again. Its first content size is 840 × 560 design units, positioned like the
former centred popup (18% down the main client area, clamped to its edges).
The minimum size is 800 × 360. Content fills the client area on resize.
The header's top left switches the shown track: `<` and `>` step to the
previous and next strip in the mixer's order (channels, busses, masters, each
in track order; `views::stripOrder`), wrapping at both ends, and the title
(`FX: <track>`, with a caret) opens a menu of every strip in that order,
divided by type, the shown track checked. A switch keeps the Chain or
Parameters tab; from a take's chain it goes to a track's.
Its Add plugin browser, chain/preset/pins menus and parameter MIDI learn
dialog belong to its own root. A strip's standalone "+" / `ui.fx.add` browser
belongs to the main root; the strip's "+" adds to its own track, the `ui.fx.add` action (the actions
list, a key) to every selected track (one undo step). Neither asks for a track. Both browsers reload shared favourites on open and
immediately before applying a toggle, preserving edits from another still-open
browser. A failed reload reports an error and does not overwrite the file.
For MIDI learn's "when focused" gate, the FX OS window must have keyboard
focus and a node in that root must be focused (including a nested dialog).
Project replacement closes every FX-root popup (browser, menus and learn),
hides/clears save prompts, and resets pending track/take/effect requests before
refreshing the new project, even when its ids match the old ones. The input
boundary checks the generation too, closing stale overlays before another
floating-window event in the same SDL batch can invoke them.


Native editor parents are tagged X11 XIDs, Cocoa NSViews or Win32 HWNDs
(ADR-0040). Local CLAP and VST3 views embed into an SDL editor container.
Windows bridged views use the same HWND across processes. macOS bridged views
float in a bridge-owned NSWindow; no empty SDL editor window is created.
The bridge applies resize/show/hide and user-close itself and reports closure
through its existing notifications. Replacing a Cocoa bridged instance opens a
fresh child-owned window; the other editor paths retain their SDL container.
Cocoa bridge placement is remembered like any editor's, in
the UI file's `plugin_windows` section: the bridge reports its window's content
frame (points, top-left origin of the primary display, the space SDL positions
use) as it moves or resizes and when it closes. A host-requested close stores
the final reply before removing the editor entry, including when no service
pass has observed its frame yet. `PluginWindows` stores it per
effect, arms a UI-file check when it changes, and on reopen sends it back with
the open request when `restoredEditorFrame` finds it still on a display (else the
bridge places the window itself; a size is sent only for a resizable editor). The
stored form is unchanged, so older projects load as before and files carry
over between modes. Cocoa bridge focus is not
included in the DAW's MIDI-learn focus predicate. The DAW sends
its owner's NSWindow.windowNumber as the Floating handle. Bridge windows use
normal level and order just above that number on open/reopen without activating
the bridge or taking key focus; other applications can cover them normally.
`PluginWindows::appActivated()` repeats this ordering for open Cocoa bridged
editors when the DAW's main window regains focus (`SDL_EVENT_WINDOW_FOCUS_GAINED`
in the GUI main loop). Cross-process transient ownership is unavailable. Windows keyboard accelerators/IME depend on the
plugin's native child; the DAW does not forward shortcut messages.

Window sizes/placements remain design units. Pixel editor sizes on
Linux/Windows use pixelRatio when converting observed geometry/placements;
container sizing goes directly from editor pixels to SDL window coordinates
using pixel density, avoiding integer design-unit rounding at fractional DPI.
Cocoa editor sizes are already points.
The GUI worker's inputScale maps SDL event coordinates to design units. Opening
uses the window's current pixelRatio; when it later changes (the editor window is
dragged to a monitor with another scale, or the system scale changes, which SDL
reports as a display-scale or pixel-size event), `PluginWindows` tells the
plug-in through `Instance::setEditorScale` and gives the window the size it
answers with, local or bridged, CLAP or VST3. The scale change is not treated as
a user resize, so the editor keeps its logical size. This is active where
pixels are the editor unit (Windows, and X11 where SDL reports a changed
pixelRatio); Cocoa editors use points and are never sent a factor
(sandbox.md §1).
Native run loops deliver local editor input; timers remain GUI deadlines.
Linux/macOS FD readiness uses the shared watcher; Windows advertises no CLAP
POSIX FD support. Display tests remain opt-in, never enabled by normal CI.


**Rack devices.** A channel that racks feed (a route from a rack's Out
module to the channel, as its record input) shows each such rack first in
its Chain tab, above the input chain, as a Rack device (`RackDevice`,
`daw/gui/app/rack_device.h`): the rack's name, "rack", and Open (the rack
window on that rack). Below come the knobs of its Macro modules
(`com.suppleaudio.rack.macro`), grouped under each Macro's label (else
"Macro"). Only a knob whose output (`macro_N` out of `out_N`) is cabled
is shown, named after what its first cable reaches: the far module's title
and its knob's or input's name, the name's first letter lower-cased unless
it is an initialism ("Filter cutoff", "Amp CV"); a plug-in parameter by
its name as the plug-in writes it ("Bracken Mod Rate"); and "+N" for the
output's other cables. Plug-in parameter names come from
`racks::PluginParamCache`, shared with the rack window's sockets: a
plug-in is asked once per fx (a scratch instance) and again only when that
fx holds another plug-in or version. A Macro with no cabled knob is left out. With none, the
device is only the name and Open: the GUI does not guess at other
"performance" knobs. A knob's drag runs `module.set` as one undo step (its
gesture); a double click resets it; its right-click offers a rack knob's
menu (Default, Automate or Show/Hide automation lane, MIDI learn… and
Remove MIDI learn, `racks::knobMenu`, shared with the rack window). The
knobs follow their Macro's telemetry, so automation moves them as it
plays: the window reads the shown Macros' telemetry every 33 ms
(`FxWindow::nextDeadline`) and watches only those modules. Telemetry
subscriptions are shared with the rack window (`racks::Watchers`: a module
is watched while any view reads it). The devices are rebuilt with the
chain when the project changes (a cable, a label), not while a knob is
being dragged. The window is this home because it is where a channel's
devices are shown whole, one channel at a time with room for eight knobs
in a row, and it already switches channels and offers a parameter's
learn and automation. The mixer strip's chain is one line per plug-in.

Chain list with add (searchable browser over the plugin scan cache;
favourites and stock plugins first; a stock plugin's row says what it is
after its name, from its first CLAP feature after the main category
("Ghyll (reverb)", "Yarrow (eq)", "Marram (synth)"), and the search matches
that too; the browser also opens on its own from a
strip's "+", titled with the track and with its own Close), reorder by
dragging a row (an insertion bar; Escape cancels; Alt+Up/Down, the
`fx` section's `ui.fx.move_up/down`, move the selected effect; no arrow
buttons), bypass vs offline
(distinct, `reaper-ux.md` addendum), a double click on a row opens that
effect's own GUI; a left double click on empty chain space (including the
"No effects yet" message or below the last row) opens Add plugin, as in the
mixer. The input section's empty area adds input FX; a take adds take FX.
Single and right clicks on empty space do not open the browser. Ctrl+click adds a row to the selection (or takes it out)
including the active row; effects moved out of the displayed track/take chains
leave the selection. Shift+click selects the rows from the last one clicked; on a row of a
multi-selection, bypass, offline and Remove act on every selected effect in
one undo step (the editor button and a double click open only that row's
editor), wet/dry, per-FX latency and CPU, preset
menu, and the plugin's own GUI (native view/child of an SDL window, or a
bridge-owned Cocoa window; ADR-0040, `clap.timer-support`, resize negotiated from observed
geometry). For plugins without a GUI — and always as a second tab — a
**semantic parameter panel** generated from the plugin's semantic schema
(units, ranges, enums, groups): the same description an agent reads, so the
human and the agent see the same names and units. Stock plugin editors are
the template's Pugl editors. Every writable parameter row of the panel
(semantic, and a plain plug-in's listed parameters) has a **MIDI** button
that names its learned control ("CC 21") or opens MIDI learn (ADR-0027): the
learn dialog says "Move the MIDI controller you wish to assign", shows the
control moved last (kind, number, channel, input), offers the CC mode
(absolute, relative 1/2/3, toggle), any input or only that one, enable only
when the effect's track (or a take FX's item) is selected, when its FX window
or editor has the focus, or when one of them shows, and soft takeover; OK
binds through `fx.learn.set`, Remove (when bound) through `fx.learn.remove`.
The same dialog learns a strip's, a send's or a VCA's own control (ADR-0058,
from the control's right-click menu, §4.2, §4.4, §4.6): it offers soft
takeover for a level or pan, and a button's CC mode starts at toggle; OK binds
every target in one undo step (`track.learn.set` / `route.learn.set` /
`vca.learn.set`), Remove frees them.
MIDI learn is one app-wide session across roots: opening another dialog closes
the first and its nested menus. Owner tokens prevent deferred destruction of
an old dialog from disabling the new listener. While the dialog is open
nothing learned moves. Latency badges refresh after effect model edits,
host replacement and live latency changes. File-descriptor service startup
and poll failures are reported; editors requiring unavailable FD service
cannot remain open. App instances use the app-wide local descriptor snapshot; caller-owned editor
instances use their editor serial. The registrations share one watcher, including
when no editor is open. Watches suspend before model/input commands can close
or reuse descriptors, and a poll overlapping a changed set is discarded.
App timers are serviced once by EngineApp; editor service adds deadlines only
for caller-owned local instances. Readiness reaches its original registration. Plugin show/hide requests change the parent window's
visibility while retaining the editor and its timer/FD service. Close takes
precedence; when show and hide are coalesced together, hide takes precedence.
Opening an existing hidden editor shows and raises that same window. Each
effect's editor reopens where it was last, at its last size when it is
resizable (a plug-in may adjust the size it is offered); placements are kept
per effect in the UI file's `plugin_windows` section (`{fx id: {x, y, width,
height}}`), so they survive closing the project. A position no longer on any
display (a monitor that is gone) is not used. The header's **Save chain…**
shows a name field for a chain preset (fx.chain.save) and **Load chain…** a
menu of saved presets (fx.chain.list; a click appends, Shift replaces);
pressing either button again while its field or menu shows closes it. A right
click on a preset in the menu offers Delete (fx.chain.delete). Each
effect's row has a **presets** button beside params and Remove (plugin
editors keep no preset bar of their own): its menu lists the plugin's factory
presets under "Factory" and the user's under "Saved" (fx.preset.list), then
"Save preset…". A click loads one (fx.preset.load, one undo step); "Save
preset…" shows a name field under the header (fx.preset.save; a name that
exists asks for a second Save, which replaces it). A saved preset has a
delete mark at its right, and a right click on it offers the same Delete
(fx.preset.delete); factory presets have neither. Pressing the button again
closes the menu. Commands:
`fx.*`, `plugin.*` (scan/describe), `param.*`, `patch.*`.

### 4.6 Routing view and matrix

The routing matrix is one separate utility window. Its first content size
is 860 × 560 design units (minimum 800 × 360), positioned like the former
popup: centred from the View action, or below the strip's "out" button with
above-anchor fallback and edge clamping. Remembered geometry takes precedence.
The grid expands with the client area; its channel menu opens in the same
window. The panel Close button, WM close request and unhandled Escape all
close this panel without quitting the app.


One grid (`reaper-ux.md` addendum 2): sources (strips in hierarchy, hardware
inputs, MIDI inputs) × destinations (parent, strips, plugin keys, hardware
outputs). A click selects a cell, Ctrl+click toggles one, Shift+click takes
the rectangle from the anchor (the last cell clicked); a click on a row's or
column's name selects all of it (Ctrl adds it, or removes it when it is all
selected; Shift takes the rows or columns from the last one clicked), and the
corner or Ctrl+A every cell. The selection is kept by source, pair and
destination across reloads. A double click on a cell without a send adds one
(`route.add`; the parent's cell turns the parent send on or off). **Add**
does that for every selected cell that can take one and **Remove** (also
Delete) removes every selected cell's sends (the parent's: the parent send
off), each one undo step. The controls act on every send in the selection,
one undo step per edit: tap, a horizontal fader for the level and a pan
control (the strip's own widgets; a drag moves each send by the same amount
from where it began, silence staying silent, and a one-shot set gives all the
same value; `route.set` with a gesture, so it records automation like the
fader), channels (the choices of the anchor's send; each goes to the selected
sends from the same source pair whose destinations have those channels),
polarity and mute (on for all unless all are on). They show the anchor's
send, else the first selected; with no send they are disabled and
neutral. A right-click on the tap buttons, the level, the pan, polarity or
mute opens that control's MIDI learn menu (§4.5) for every selected send
(`route.learn.set`, ADR-0058); moving the level or pan makes the anchor's
send the one "Routing: last touched send (MIDI CC)" follows (§6). Cells that `route.strip_type` would reject are drawn
disabled with the rule as their tooltip; record-input assignments appear in
the same grid. A graph view (nodes and edges, PDC per node) is P1. Strips are listed in the
views' order (`route.matrix`): each master's tree with a parent's channels
before its busses, then the masters at the bottom, under the views' labels
(§4.2). A strip's "out" button opens it (TCP and mixer). A strip with
auxiliary channels (a multi-out instrument's outputs, ADR-0021) has a row per
channel pair under its own, named by the outputs ("3/4 Kick"); a cell there
sends that pair to the destination's main pair, and its channel menu keeps
the pair while choosing the destination's.
Matrix updates clamp scroll to the current endpoint range; paint uses
endpoint-pair indexes for send and cycle rules.
Commands: `route.*`, `track.set {record_input}`.

### 4.7 MIDI editor

Piano roll (instanced notes; draw, select, move, resize, velocity drag,
quantize, transpose), velocity and CC lanes, and a **notation panel**: the
item's notation text (`libs/notation`) beside the roll, editable with live
compile diagnostics (the compiler's located errors and hints); edits in
either side update the other through `midi.edit` (event edits) and
`midi.write` (notation replacement). A draft that would replace MIDI changed
elsewhere since it was loaded is not written by the pause, leaving the panel or
closing the editor: the panel says so, and Ctrl+Enter applies it over the
change. This is the
LLM-native differentiator: the same text an agent writes is visible and
editable by the human. The event list edits notes and CC events;
double-click editing requires a row hit and edits that row. Roll gesture
anchors use exact model timing rather than the rounded `midi.notes` query.
A right-click on the grid (on a note: that note joins or is the selection)
opens **Delete**, **Copy**, **Cut**, **Paste** (at the pointer's beat on the
grid, pitches as copied) and **Duplicate** for the selected notes; the same
actions (`ui.midi.notes.*`) have the arrange view's keys in the `midi`
section: Delete, Backspace and Alt+C, Ctrl+C, Ctrl+X, Ctrl+V (at the
playhead), Ctrl+D and Ctrl+A. The rest of the editor's keys are actions in
that section too: Up/Down transpose the selection a semitone (Shift: an
octave), Left/Right move it a grid step, Q quantizes it (every note when
none is selected), Escape clears it. With the event list shown, the delete
action removes its selected rows, notes and CC events alike.

The panel's top border is a drag handle: dragging resizes the panel (up is
taller; up to nearly the whole window, always leaving the transport and a strip
of arrange, and clamped again whenever the window shrinks or a neighbouring
dock grows), double-clicking resets it to the default height. The height is
UI state (the `midi` section of the sidecar, §1.2). Wheel in the roll, as over
the arrangement: zooms time around the pointer (a sideways wheel scrolls time);
Shift scrolls time; Ctrl scrolls pitch; **Alt zooms the key/row height** around the pointer's pitch (4–48 px per key), and also works
over the keys, lanes and header. Row height is not persisted. These are the
defaults of the `midi.wheel` mouse context (§6), which can bind any
combination to scroll the pitches, scroll in time, zoom in time or zoom the
rows.

Native file-dialog completions check owner lifetime before posting and again
on the UI thread; outstanding dialogs may safely outlive shutdown.

### 4.8 Render dialog

A front end to `render.*` (`engine.md` §3, ADR-0055). Every control maps to a
render-settings field (session-model §1.10). Nothing is GUI-only except the
"ask" choice for existing files (below) and "Open folder when done" (a
preference). On open, the dialog reads `render.settings` and shows the
effective values. A value that comes from the user default rather than the
project is drawn in the quiet ink, with "from your default" on hover.

**Layout**, top to bottom; scrolls when taller than the window:

- **Header:** a Preset dropdown (factory presets, then the user's, each
  loaded into the dialog only, written to the project only on Render),
  **Save preset…** (asks for a name), **Delete preset**, Close.
- **Source:** a segmented control: Master mix | Stems | Regions.
  - Master mix, when the project has more than one master, shows a checklist
    of the masters (All / None; the main master alone by default) that edits
    `masters`: each checked master renders its own file (ADR-0056). Unchecking
    every master returns to the main master.
  - Stems shows a checklist of top-level tracks (All / None), plus Mode
    (Isolated / Through master) and Tap (Post-fader / Pre-fader / Pre-FX)
    dropdowns and a "Skip silent stems" checkbox.
  - Regions shows a checklist of regions, Handles (an editable dropdown in
    seconds: 0, 0.5, 1, 2) and **Matrix…**, a grid of regions × tracks and
    masters that edits `matrix`.
  - Version: a dropdown with Current, each other branch's head, and "Earlier
    version…", which picks from `history.list`. The choice applies to this
    render only and is never saved.
- **Range:** a dropdown with Entire project | Time selection | Custom.
  - Time selection is the loop range drawn on the ruler (`bounds: loop`),
    whether or not looping is on; it is disabled while there is none.
  - Start, End and Length are shown in the ruler's format, and are editable
    under Custom.
  - Tail: a checkbox and an editable dropdown in seconds (0.5, 1, 2, 5, 10),
    with "Cut silence at the end" (`trim.end`).
- **Output:**
  - **Folder (all projects)**: the path (`~` for home), **Choose folder…**
    (the system dialog; the `render_directory` preference) and **Open**,
    which shows it in the file manager via SDL's URL opener.
  - **File name**: the pattern field, with **Tokens ▾**, a menu that inserts
    §7.3 tokens at the cursor.
  - **If the file exists**: a dropdown with Overwrite / Add a number / Skip
    / Ask. Ask runs a dry run first, then asks with the list of existing
    files and continues as Overwrite or Skip.
- **Format**: one tab per output (Primary; "+ Output" adds one, up to four;
  each added tab has a remove ×):
  - **Format** dropdown: WAV, AIFF, FLAC, MP3, Ogg Vorbis.
  - **Bit depth** dropdown, listing only the format's depths: "32-bit float"
    for WAV.
  - **Dither** dropdown (Off / TPDF / Noise-shaped), shown only for 16 and
    24 bits.
  - **Sample rate**: an editable dropdown with "Project (N)", 44100, 48000,
    88200, 96000, 176400, 192000; any typed integer from 8000 to 384000.
    Later tabs add "Same as primary".
  - **Channels** (primary tab, applies to all outputs): Stereo / Mono, plus
    "Mono tracks as mono" for stems.
  - MP3: Mode (Constant / Variable), Bitrate (the CBR list) or Quality (V0–V9).
  - Vorbis: Quality (−1 to 10).
  - Later tabs: Suffix.
- **Loudness** (folded unless set; the fold line summarises, e.g. "−14 LUFS,
  −1 dBTP"):
  - Normalize dropdown: Off / Integrated loudness (LUFS-I) / Loudest 3 s
    (LUFS-S) / Loudest 400 ms (LUFS-M) / Sample peak / True peak / RMS.
  - Target: an editable dropdown whose list and unit follow the mode. LUFS:
    −14 (Spotify, YouTube), −16 (Apple Music, podcasts), −23 (EBU R128), −24
    (ATSC A/85). Peaks: −1, −0.1. RMS: −20, −16.
  - "Only turn down".
  - For stems: "Same gain for every stem (from the master mix)".
  - "Limit true peak to" a checkbox and an editable dropdown (−1, −0.5, −0.1,
    −2), usable with Normalize off.
  - "Write a loudness report".
- **Processing** (folded unless set): Trim start / Trim end and their
  threshold (−60, −70, −50 dBFS, editable), and "Seamless loop (fold the tail
  into the start)".
- **Metadata** (folded unless set): Title, Artist, Album, Track number, Date,
  Genre, ISRC, Comment (tokens allowed), Cover (a file chooser that starts in
  the project folder), "Write BWF timecode and loudness (WAV)" and "Embed
  markers and regions (WAV, AIFF)". Formats that cannot hold a field say so
  beside it.
- **After render:** "Add to project" and "Open folder when done" (a
  preference).
- **Preview:** the live dry-run list (re-run on every edit, coalesced per
  frame): each file's name, duration and outputs, "exists" where it does,
  and a count, in a fixed-height list that scrolls (stems and regions can
  list many files). An invalid field blocks the preview, queue insertion and
  rendering with a field diagnostic that names the field.
- **Buttons:** Queue (n), Add to queue, **Save as default**,
  Render N files.
- **Results** (after a job): for each file, its outputs, LUFS and dBTP, the
  checks as "ok" or "FAIL" with the measured value (the UI fonts have no check
  marks), and the skipped files with their reason, in a scrolling list like
  the preview's. Below it, a file picker and **File actions…**, whose menu
  offers Open folder, Add to project, and "Branch from this version", which
  runs `branch.create {version}`.

**Writing the settings.** Render and Add to queue compare the dialog's
settings with `project.render`, canonically. If they differ, they write them
first with one `project.set`, labelled "Render settings" in history, then
render. The one-off Version choice and "Ask" are never written.

The dialog tracks one render or queue job at a time; a second launch is
refused with a status message while it tracks that job. Cancelled jobs remain
labelled cancelled and retain any partial result files returned by the
engine.

**File → Mix** (the action `ui.render.mix`; a menus file that still lists
`render.mix` runs the command directly) runs `render.mix` as a job with no
other arguments, so it follows the project's settings, then the user default. Its menu
label is plain "Mix" (a label naming the file and format made the File menu far wider).
Progress shows in the status bar, then the path of the file written
(the status bar holds text only); "Open folder when done" (§4.11) opens the
folder.

### 4.9 Recording

Arm, input and monitor in TCP and mixer; record mode (normal, punch,
loop takes, ADR-0014/0015) in the transport.

### 4.10 Command log and history

A dock showing the command log (every command with origin — GUI, agent,
Lua, MCP — arguments, result/error) and the version history per branch
(undo/redo, jump to version, branch list). Agent commands are visibly
attributed.
The history panel refreshes on changes to the project generation, head
version, branch list or current branch name, including branch changes that
leave the head version unchanged.

### 4.11 Project files, folders and templates

File > New, Open…, Save, Save as… and Import… use the system's file
dialogs (asynchronous; results run on the UI thread), and ask Save / Don't
save / Cancel before unsaved changes are lost. Before New, Open or Quit checks
for unsaved changes, active recording is stopped through `transport.stop`:
recorded takes become an undoable version, then the normal prompt runs. If
Stop fails, the error is shown and the requested action does not continue.
Import… (`ui.file.import`, Ctrl+I) takes audio files (WAV, FLAC, MP3) and
Standard MIDI Files (`.mid`, `.midi`) through `item.add` (`audio_file` or
`midi_file`), at the playhead in one undo step: one file onto the selected
track, else each onto a new track named after it. Files dropped on the
arrange view are told apart the same way. An imported MIDI item plays through
its channel's instrument, so a new track needs one before it is heard.
File > Quit, its shortcut (Cmd+Q on macOS), SDL quit and main-window close
all dispatch the same `ui.quit` action. An open popup only gates interactive
Quit when recording or unsaved changes require a decision; a clean, idle
project can quit immediately. Hidden/finite runs still stop recording, even
though they suppress the prompt and ignore the popup gate.
Discard decisions belong to the project generation that opened the prompt.
Every button, including Cancel, checks that generation before stopping recording,
saving or continuing. If the project was replaced, the old request is cancelled
with the same short notice as a stale native dialog result.
Discard decisions also capture the session version and branch. Save, Don't save,
Save As completion and the final continuation recheck recording and that
revision. A recording started while the prompt/dialog was open is stopped;
a Stop error aborts. If recording was stopped or the version/branch changed,
a fresh prompt replaces the old decision before saving or continuing, even
if an intervening save made the new revision clean.
Native file/folder results capture the project generation and are dropped with
a short notice if the project was replaced before delivery. Open rechecks the
session version and branch after the dialog; changes require a fresh discard
decision. Recording started meanwhile is stopped before that decision too.
A save (or project template)
that comes back with warnings (a plug-in whose state could not be read keeps
its earlier state) opens a popup listing them. Open and Save as start in the
project's own file, else in the media folder `~/Documents/<slug>-media`
(created on demand); any other folder can be browsed to. Renders go to the
same media folder unless the render dialog chose another (§4.8); the render
folder is one for all projects (ADR-0055). Under `--workdir` (tests) they go
to `<workdir>/renders`.

**Each project has a folder of its own**: Save as of `<folder>/<name>` saves
`<folder>/<name>/<name>.json`, with the project's `media/` beside it, unless
the chosen folder already is `<name>`; a new name chosen from inside the
current project's folder goes beside that folder, not inside it. Saving as
onto another existing project is refused with a message. With **Copy
imported audio into project** on (Preferences, the default), imports keep
their own copy in the project's media folder (session-model.md §5.3).

**Project templates** are whole projects kept by name in
`<config>/<slug>/templates/<name>/` with a copy of their audio
(project.template.*): File > Save template… asks for a name (a name in use
asks once more, then replaces it); File > Load template… lists them, a click
starts a new, unsaved project from one (after the unsaved-changes question)
with its own copy of the template's audio, and a right click offers
Delete.

**Track templates** keep one strip — its settings and both FX chains, not
its items or routing — by name in `<config>/<slug>/track-templates/`
(track.template.*): Track > Save as template… asks for a name for the first
selected channel or buss, as above; Track > Add from template… lists them, a
click adds that strip last under the master and selects it, and a right click
offers Delete.

### 4.12 Racks (ADR-0062)

The rack window (`daw/gui/app/rack_*`) is a separate, resizable utility
window, as the routing matrix's, its placement in the UI sidecar
(`windows.rack`). View → Racks and the toolbar's Racks button (`ui.rack`)
open and close it; `ui.rack.show {track}` opens a channel's first rack and
`ui.rack.add {track?}` adds one (on the given or selected channel, else on
a new channel). It follows rack.md §8.

- **Tabs.** One per rack, its home channel's colour as a dot and its label
  (its name, else the channel's); `+` adds a rack. A double click renames
  (`rack.set name`); a right-click offers Rename, the home channel and
  Remove rack. The window's title names the rack shown.
- **The space** is the theme's ground and nothing else; an empty project
  says only "No racks yet. + adds one on the selected channel." A rack that
  holds only its own Outs and Ins invites a start: under them, in quiet
  ink, "Add a module, or start from a patch", two links that open the
  browser (shown if hidden) on its Modules or its Patches view with the
  search focused; the line goes with the first other module. Middle-drag
  or Space-drag pans (Space alone, released without a drag, still plays and
  stops), Ctrl+wheel zooms at the pointer from 10 to 400 %, the wheel
  scrolls (Shift: across), Ctrl+= and Ctrl+- zoom, Ctrl+0 is 100 %, Home
  shows everything (at most 100 %, as far out as it takes) and Shift+Home
  fills the view with the selection (at most 150 %). Below 55 % the faces'
  small words go and each module's name is drawn over it at 10.5 points on
  its title colour, across its top when it fits and down its length when
  the module is narrow; the hovered and selected names are outlined. The zoom readout in the header toggles 100 % and
  everything. Each rack's view (offset, zoom) and the rack shown are in the
  UI sidecar (`racks`); a rack without one opens showing everything.
- **Modules** are drawn from their type's face (rack.md §4.5, or the
  module's `panel()`): a title strip with the label (the type's name; an
  Out or In module's `moduleLabel`), knobs, toggles, choices, sliders,
  readouts, displays, and jacks. A knob's or jack's name sits under it in
  a cell of its own (`rack_labels.cpp`): the line stops short of the
  controls beside it, and two names on one line split the room between
  them at one point (each its need, the rest shared). A control right
  under the name shortens the line; one its width would reach shortens it
  to no less than 8.75 units, below which the name narrows instead. A name
  that does not fit its cell is drawn smaller (8.5 down to 7.25), then on
  two lines where the space under it is free, then cut with an ellipsis;
  a hovered knob's value shows in the same cell. Whatever the layout,
  automatic or a module's `panel()`, no name runs into another or under a
  control (a test holds every stock and host face to it). Other words too
  long for their place (toggles, choices) are drawn smaller, down to three
  quarters of their size, or on two lines, before they are cut short. An
  Out module carries a tag under it naming the channel it feeds. A plugin module shows the plug-in's name and
  vendor on its plate and a window mark; a click (not a drag) opens the
  plug-in's editor, as the mixer's slot does. A module of an unknown type is
  kept, outlined in the clip colour, saying "Missing module" and its id.
  Bypassed modules are veiled and say so. Modules have a soft shadow and
  are dragged by any part that is not a control: a click selects, Ctrl+click
  or Shift+click toggles, a drag on the space draws a marquee (Ctrl or
  Shift adds), and the selection moves together as one undo step
  (`module.set x, y` in one transaction); the arrow keys nudge it 10 units
  (Shift: 1), one undo step a press. Delete removes the selection; Ctrl+A
  selects every module. Ctrl+C copies the selection with the cables among
  its modules, Ctrl+X cuts it, Ctrl+V pastes with the copy's top-left at
  the pointer (else the middle of the view), selected and in view, and
  Ctrl+D duplicates the selection, cables too. Copy and cut are
  `module.copy` (the engine's module clipboard, shared with agents: the
  modules as they are at the copy, plug-in state and learn bindings
  included, and the cables among them; a cut deletes only after the copy
  succeeds). Paste is `rack.paste {rack, x, y}`, one undo step, the same in
  the rack the copy came from, in another rack and after a cut; Paste is
  enabled while the clipboard holds modules. A right-click offers Open editor (plugins), Rename…,
  Bypass, Copy, Cut, Duplicate, Randomize, Presets… (the type's factory
  patches, `module.set preset`), Show source, Capture notes to track…
  (modules with pitch and gate outputs), Save as patch… (the
  selection) and Delete. Capture notes to track… picks a channel, then a
  length (1, 2, 4, 8 or 16 bars, from the bar the playhead is in) and runs
  `rack.capture` with the module's first pitch and gate outputs (and its
  `velocity` output, if any): one undo step, a MIDI item on that channel
  at that bar; the status bar says how many notes it caught. On the space: Add module…, Add plugin… there,
  Load patch… (the browser's Patches view), Save rack as patch…, Paste, Tidy up, Show
  everything and Show selection. Tidy up is `rack.arrange` (one undo
  step): with two or more modules selected it reads "Tidy up selection"
  and moves only them, from their top-left, then brings them into view;
  otherwise it lays out the whole rack and shows everything.
- **Adding at the pointer.** Typing a letter or digit over the space, a
  double click on it, or its menu's Add module… opens a search list there
  ("rack.add": the browser's entries, Plugin… and the rack patches
  included, a name starting with the text first, then a word of the name
  starting with it, then a name containing it, then its id, tags and
  description). Enter or a double click adds the choice with its top
  centred at the point, selected. A new cable dropped on nothing opens the
  same list (without Plugin… and presets), and the module chosen takes the
  cable: on its first input of the cable's signal, else its first input
  (from an input: its first output of that signal), the module beside the
  point on the cable's side, the module and its cable one undo step.
- **Jacks** are circles ringed in their signal kind's colour (§7); outputs
  sit on a plate, as on hardware. A jack glows by its level from the
  module's telemetry (an input by its cables' sources'); a poly output shows
  its channel count. Dragging from an output draws a cable that follows the
  pointer, and the jacks it may end on answer with a ring; dropping on an
  input connects (`cable.add`), on a knob makes a modulation cable. Dragging
  from an empty input draws one backwards to an output. Dragging a
  connected input picks up its last cable by the plug: dropped elsewhere it
  moves (one undo step), dropped on nothing it is removed.
- **Plug-in parameters.** A cable from an output dropped on a plugin
  module's body (which outlines itself and says "Drop to modulate a
  parameter" while a cable is over it) opens a searchable list of its
  plug-in's modulatable parameters, by name with their group; Enter or a
  double click makes the cable (`cable.add` to `fx_N.<key>`: the semantic
  key, else the CLAP id; amount the command's default, half the range).
  A VST3 plug-in's list says it takes no modulation, as does a plug-in with
  no modulatable parameter. Each parameter the module's cables reach is a
  socket on its top edge, left to right in the order of their first cable
  (closer together when many); hovering one, or a cable into one, shows the
  parameter's name and amount over it ("Cutoff · +50 %"). Its amount is an
  arc around the socket (270 degrees at ±100 %) and drags like a knob's
  ring: 160 points from −100 % to +100 % (Shift finer), one undo step.
  Dropping another cable on a socket adds it to that parameter.
- **A cable's right-click** (on its cord, or on a plug-in parameter's
  socket) names what it reaches ("Cutoff on Synth"), offers Amount… for a
  modulation cable (a field: a plug-in parameter's in percent, a knob's in
  its modulation unit; `cable.set`), Change parameter… for a plug-in
  parameter's (the list again; the cable keeps its amount), and Remove
  cable.
- **Cables** hang between their ends, deeper the longer they are, drawn
  above the modules in their output's signal colour (or the cable's own
  colour), with a dark edge and a highlight, plugs at both ends; a
  modulation cable plugs into its knob's rim. Poly cables are thicker. A
  cable glows around its cord, and its core brightens, as strongly as its
  source output's level (the jacks' measure; a pitch cable only faintly);
  a cable that crosses the view glows from an out-of-view source too: such
  a source is not watched, its outputs' block peaks are read
  (`EngineApp::moduleOutputLevel`, a gate on when its peak is) while the
  transport plays, and are zero when stopped. While a module or a cable is hovered, or modules are selected, their
  cables are drawn at full strength and on top and the rest step back (to
  12 % with eight or more cables, else 45 %); with nothing in focus, eight
  or more cables are drawn at 70 %. While a cable is drawn, the others dim. A cable that closes a loop (`rack.get`'s
  `feedback`: it reads the previous frame) is dashed over a dim cord.
- **Knobs** drag vertically, 160 points end to end at any zoom (Shift:
  ten times finer), along the travel MIDI learn turns them on
  (`rack::knobPosition`/`knobValue`: a frequency's logarithmic), double-click resets, and show their value while hovered
  or dragged; a drag is one undo step (`module.set params` coalesced by
  gesture). Choices and short integer ranges show their steps as dots;
  a choice drawn as a box steps on a click. Modulated knobs carry a ring
  outside the travel: faint for the range the cables' amounts span, bright
  to the value they make now (telemetry); dragging that ring sets the last
  cable's amount (`cable.set`). A right-click offers the default value, a
  choice's options, and Automate: an envelope on the knob holding its value
  (`env.add {module, param, points}`), shown as a lane under the rack's home
  channel ("Rack › Module · knob", on the knob's own scale); once there, the
  item shows or hides that lane. MIDI learn… opens the learn dialog (§6)
  for the knob: it listens, and OK binds what it heard (`module.learn.set`;
  a toggle's CC defaults to toggle mode, others offer soft takeover). A
  bound knob carries a small accent dot in its gap (a control, at its
  corner), and its menu names the controller ("MIDI: CC 21 · ch 1…") and
  offers Remove MIDI learn (`module.learn.remove`). The controller moves
  the knob through `module.set`, so the window's knob follows it along the
  same travel (tested end to end with a dummy device's CCs). A plugin
  module's knobs are its plug-in's (the FX window's learn).
- **Outside the rack.** A rack's Macro modules are its performance
  controls: their cabled knobs show in the FX window of each channel the
  rack feeds, as a Rack device with Open (§4.5).
- **Browser.** A column on the left (the header's Browser button shows or
  hides it) with two views, Modules and Patches, side by side at its top.
  It opens on Modules, which lists module types only, so what it shows
  does not move as patches are added. Modules: a search field (words match name, id, description and tags),
  the catalog's tags as chips that filter (one row: the chosen tag, then
  the most used, and "+N", which shows every tag alphabetically with Less;
  picking a tag folds them again), and the types by origin (host,
  stock, library, project) with their one-line description, Plugin… first.
  A double click or Enter adds the type in the middle of the view; dragging
  a row into the space adds it where it is dropped. Plugin… (and Add
  plugin…) opens the plug-in browser; the plug-in chosen becomes a plugin
  module (`module.add {plugin}`), with note jacks when it is an instrument.
  A right-click on a row offers Add to rack and Show source. While a row
  of either view is hovered, a card beside the browser shows its whole
  description (and a module's tags; a patch's kind, module and cable
  counts and module types).
- **Patches** are rack presets (`rack.preset.*`, rack.md §7.1), on the
  browser's Patches view: its own search (name, description, module
  types), no tag chips, the user's saved ones under "Yours", then the
  factory patches under "Factory" (`rack.preset.list`'s `factory: true`),
  each with its description, or its module count and types. A double click
  or a drag loads one into the rack shown (`rack.preset.load`; a drag at the
  drop point, else beside the rack's modules), its modules selected and
  brought into view. Into an empty rack (only its own Outs and Ins) a
  load replaces nothing: the patch's Out and In are the rack's own (`io:
  join`), moved to the patch's own places, so the patch keeps its layout
  (rack.md §7.1), one undo step, and everything is shown. A right-click
  offers Load into rack and, for the user's own, Delete patch (a factory
  patch of the same name comes back). Saving (Save as patch…, Save rack
  as patch…) asks a name in a field over the space (the rack's label, or
  the one module's) and
  calls `rack.preset.save` with the selection's ids or none; a name in use
  asks Replace "<name>" before saving with `replace`. The list is read when
  the window opens and after each save or delete.
- **Source.** Show source opens the type's code (`module.get {type}`) in a
  code view over the window, titled with its name, origin and id. A library
  module's is editable: Save or Ctrl+Enter writes and compiles it
  (`module.write`), racks using it reload, and a mistake is marked on its
  line, the caret moved there and the message said under the code. A stock
  or project module's is read-only and offers Make editable copy
  (`module.fork` to `com.library.<name>`), which opens the copy.
- **Telemetry.** While the window is open the visible modules are watched
  (`Instance::watch`) and read about 30 times a second; hidden or scrolled-
  off modules are not.
- **Displays.** A module with `draw()` draws itself under its controls,
  in its own coordinates, through the window's copy of the module: its knob
  values and the `Shared` state the telemetry carries. Its palette is the
  app's theme. A hook that spends its budget stops and says "draw() ran too
  long"; the rest of the window draws on. A module with `pointer()` hears
  the mouse on its displays (`panel()`'s `display` rectangles) where no
  control is, in its own coordinates, as a plugin editor's does: a press,
  each move while held (a drag, wherever it goes) and the release; a hover
  and its leaving; the wheel. Its knob edits (`Param::edit`) are
  `module.set`, one undo step a press or a burst of the wheel; on a hover
  they do nothing. There a press does not move the module, and the wheel is
  the module's; Ctrl+wheel still zooms, the right button still opens the
  module's menu and the middle one still pans. A module without `pointer()`
  is dragged by its displays as by the rest of its body.
- **Elsewhere.** A channel's input menu lists every rack's Out modules
  ("<rack> › <Out>", with the home channel's colour) after the device's
  inputs, and New rack on this channel; picking one is `track.set
  {record_rack}`, and the strip's input button names it the same way. A
  channel's right-click menu offers Show rack (when it has one) and Add rack
  (Add another rack).

## 5. Agent panel (removed)

The app has no built-in agent (ADR-0043). Agents drive it from an external
harness through the MCP server (`command-bus.md` §4), with the same commands,
origins, undo history and branches. UI files that still carry `panels.agent`
load; the key is ignored.

## 6. Actions, keys, toolbars, menus

On macOS every Ctrl shortcut and pointer gesture below uses Command; labels
show Cmd, and Option is Alt. The portable `kCtrl` bit means this primary
modifier, while physical Control uses `kSuper` on macOS. Keymap documents
accept Ctrl and Cmd as primary aliases; Super retains the secondary bit.
Cmd-Q and application-menu Quit use the normal clean shutdown path. Linux and
Windows retain Control shortcuts. No custom native menu hierarchy is supplied.

- **Actions** = a command plus bound arguments (and optional argument
  prompts), named and searchable (`reaper-ux.md` addendum: ~4,400 REAPER
  actions collapse to ~2,000 shapes; parameterized commands cover them with
  far fewer entries). The registry's commands generate the base set;
  context-dependent arguments resolve from the selection ("selected
  tracks") at invocation.
- **Actions list** (`?`): fuzzy search over names, descriptions and
  commands; shows every key bound to an action (menus show only the
  first); runs, binds (Ctrl+B, then the chord), adds to
  toolbar/menu (Ctrl+T, Ctrl+M), and learns a MIDI control for it (Ctrl+L:
  the learn dialog of §4.5 with the CC mode and input choice, and soft
  takeover for a "(MIDI CC)" action that sets a value from a knob's
  position: the selected tracks' volume and pan, the master's volume and the
  playrate; with it, an absolute knob moves each value only once it reaches
  it, per track). The list shows
  a bound control after the keys. A press (note on, CC above 0, program
  change, an encoder step) runs the action once; the "(MIDI CC)" actions
  follow the value instead — the selected tracks' volume (the fader's
  travel) and pan (an encoder step is 1 %), the master's volume, a beat
  jog of the play cursor, the master playrate (§4.1), the **last touched
  send** (the level or pan the routing matrix moved last, §4.6; soft
  takeover per send), and the **last touched
  parameter** (the plug-in parameter edited last in the FX window or a
  plug-in's own editor; a knob takes over softly, an encoder at once; the FX
  window's tab row names it) — one undo step per movement. Other actions,
  the registry's commands among them, receive no value: when the learn
  dialog hears a knob or fader (an absolute CC or pitch bend sending more
  than two values) for one of them, it warns that the position is not passed
  on and names the "(MIDI CC)" action that follows it, if there is one
  (`transport.playrate.set` → Transport: playrate (MIDI CC)). A control drives one
  action (learning it for another moves it); the bindings are user settings
  in `controls.json` (`supple.controls/1.0`: `{bindings: [{action, midi,
  mode, soft_takeover}]}`, beside the keymap; `soft_takeover` is written only
  when true). Its keys work with the focus on the search
  field or on the list (after a click on a row), where typing goes on
  searching. A new binding goes to `global`; an `arrange` binding on the same
  keys gives way (the arrange view looks there first), and the hint names
  the action that had them. **Reset keys…** (also the action
  `ui.keys.reset`) asks first, then restores every default binding and saves
  a keymap file without changes. Selected-item actions cover the take
  features: explode onto tracks or in sequence, render with FX as a new take,
  in place or in place deleting the FX (`item.apply-fx:delete-fx`), delete the
  heard take, show all takes (`ui.item.takes`).
- **Keymaps:** per context section (global, arrange, MIDI editor, mixer,
  FX window, text field), chords allowed, conflicts shown in the editor. A
  key looks in `text` while a text field has focus, in `midi`, `mixer` or
  `fx` while the pointer is over that panel (else while the focus is inside
  it: the pointer is over the bars or outside the window), then in `arrange` (its defaults
  edit the shared selection) and `global`, innermost first
  (`gui::keySections`). So a key acts on what is under the pointer: Delete
  over the arrange view removes the selected items even while the MIDI
  editor has the focus. Stored as
  canonical JSON in user config; defaults close to REAPER's common keys
  where they are not proprietary assets (key choices are not copyrightable
  expression; no REAPER files are copied). Among the defaults: Space
  play/stop, R record (also Ctrl+R), L loop, Delete or Alt+C deletes the
  selection (items, tracks or points; notes over the MIDI editor), Ctrl+M or
  Alt+D the mixer,
  Alt+E the MIDI editor on the item it had last (else the selected or first
  MIDI item; nothing without one). Space is never a button's: buttons and
  toggles take Enter, so Space after a click still plays or stops, and a
  rename that ends gives the keys back to the shortcuts.
- **Macros:** a sequence of actions (or a Lua script) run as one batch (one
  undo step), with typed prompts; the values given reach every step, each
  taking the ones its own prompts name. Macros nest up to 16 deep.
- **Prompts:** an action run from a menu, the toolbar or a key that still
  needs typed arguments the selection does not supply opens the same form
  as the actions list.
- **Toolbars and menus:** built from actions; customizable; stored in user
  config (`menus.json`, supple.menus/1.0). Edited in the app: Ctrl+T in the
  actions list puts the selected action on the toolbar (or takes it off),
  Ctrl+M adds it to a menu or removes it; a toolbar button's right-click menu
  moves or removes it, or resets menus and toolbar. The default top-level
  menus end with Project, then Help. Loading an older `menus.json` appends
  Help if absent; existing custom menus, toolbar and explicit Help edits remain.
  A saved file lists the default actions it has seen (`known`); on loading,
  a default action added since takes its default place in its menu and the
  toolbar (after the nearest default before it that the list holds), and one
  the user removed stays removed. A file from before `known` is taken to
  know every default but those added since (View → Racks and the toolbar's
  Racks, `ui.rack`).
  An action may carry an `enabled` predicate over the selection context; its
  menu entry is greyed while that is false (the predicate is checked when the
  menu opens). The Item menu's entries are built when it opens
  (`itemBarMenu`, § Menus under arrange items); action ids a user adds to it
  follow them after a separator. Loading a `menus.json` drops the item ids
  older defaults listed there (and `ui.midi.new`) from its Item list, as the
  two item menus now hold them, and the track edits from its Track list
  (§4.2).
  **Mix to new track** (`ui.item.mix-to-track`, `item.mix-to-track` on the
  selected items, engine.md) is greyed without selected items; progress and the result show in the status line as for
  Consolidate items, and only one such background item job runs at a time.
- **Help** provides four app-local actions (not engine command-registry entries):
  `ui.help.skill`, `ui.help.skill_link`, `ui.help.mcp`, `ui.help.agent`.
  **Download skill…** and **Download skill link…** open the native folder
  picker directly, with no preset destination or intermediate panel. The chosen
  folder receives `<slug>/SKILL.md` and the rest of the bundled `host::skillDir()`.
  Copies are independent; links follow this installation's updates (a directory
  symlink on Linux/macOS, a native directory junction on Windows, requiring no
  administrator or developer mode). Skill links from temporary macOS App
  Translocation paths are refused until the app is moved and restarted;
  independent skill copies remain available. Existing skill folders or links
  require an explicit Replace click. Files and folders without SKILL.md are
  refused, as are destinations within, identical to, or containing the source
  skill.
  Preparation happens on a worker before replacement; a failed publication
  restores the previous target. Cleanup explicitly removes symlinks and Windows
  reparse points as links, including links inside temporary or previous exports;
  it never relies on recursive removal following platform-specific link rules.
  Success and actionable failure appear in a compact result dialog.
  **MCP server…** displays copyable absolute commands for this running copy's
  adjacent `<slug>-mcp` and `<slug>-engine --mcp`, with build-tree fallback
  (`mcp/` and `host/engine/`). A development `.app` under `daw/gui/` uses
  the same fallback when the inferred build root contains `CMakeCache.txt`;
  adjacent helpers always take precedence. It includes common `mcpServers`
  JSON, stdio instructions, app-first startup, automatic reconnect and
  `--instance NAME`.
  **Connect an agent…** offers Claude Code, Codex and Hermes, with a copyable
  registration command and Connect. Claude Code uses user scope; all three
  use the current MCP executable. Connect searches PATH, common user install
  folders, and the login shell on POSIX (including Finder launches). Windows
  searches `.exe`, `.cmd` and `.bat`; npm shims run through a temporary batch
  file with percent expansion escaped and delayed expansion disabled. The
  copied Windows command is for PowerShell. Child processes run off the UI
  thread with captured stdout/stderr, a 30-second deadline, bounded output,
  cancellation on shutdown and owned process-tree cleanup. Hermes receives a
  single blank response to enable discovered tools, but declines overwrites.
  Codex checks its structural `mcp list --json` result for an existing
  registration before its add command can replace it.
  Temporary macOS App Translocation paths block MCP/agent command copying and
  Connect. The panels explain how to move the app to Applications and restart.
  JSON preserves its copied indentation; command tokens are quoted only when
  the platform shell needs it.
  Connect also links the bundled skill as `<slug>` in the harness's user skill
  folder, shown in the dialog before Connect: Claude Code
  `<$CLAUDE_CONFIG_DIR or ~/.claude>/skills`; Codex `~/.agents/skills`, also
  checking its deprecated `<$CODEX_HOME or ~/.codex>/skills`; Hermes
  `<$HERMES_HOME or ~/.hermes>/skills`, `%LOCALAPPDATA%\hermes\skills` on
  Windows. The variables come from the app's own environment (a Finder or
  desktop launch may not see ones set only in a shell profile); relative values
  are ignored. The folder is created when missing. An existing `<slug>` entry
  in any of those folders is never replaced: a link to this copy's skill
  reports "Skill already installed"; another skill or link, or a non-skill
  entry, is reported and left in place (Download skill link replaces it). The
  link is made like Download skill link's and is attempted whatever the
  registration outcome, but not when the harness CLI is missing. Help
  paragraphs measure at the panel's inner width, so wrapped lines keep the
  buttons inside the panel.
  Panels fit their contents; longer operation output scrolls. The dialog reports
  exit status, the skill outcome and an output excerpt, including
  existing-registration outcomes; a missing harness leaves the command
  available to copy. Other clients get the skill through the two Download
  actions.
- **Mouse modifiers:** a map per context — item drag
  (move, copy, slip; each also off the grid), item edge (trim, stretch), item
  click (select, toggle, extend), automation item edge (stretch, loop), fade
  handle (drag, cycle shape), empty-lane drag (select, add), automation lane
  (edit, toggle, delete), ruler markers/regions
  (move, remove), tempo points (toggle ramp), and the wheel over the TCP
  (`tcp.wheel`) and the arrangement's lanes and ruler (`arrange.wheel`):
  scroll the tracks, scroll in time, zoom in time, resize all track heights;
  and over the MIDI editor (`midi.wheel`: scroll the pitches, scroll in time,
  zoom in time, zoom the keys and rows) —
  from each modifier combination to a behaviour. The defaults are the built-in gestures;
  `mouse.json` (supple.mouse/1.0, only what differs) overrides them, and
  Edit > Mouse modifiers edits the live map. The wheel's zoom anchor is not
  here but goes with the follow mode (§4.3); a saved `wheel.zoom` context
  (its former single setting) is ignored on load.
  TCP wheel defaults are plain scroll, and Ctrl and Alt resize all; over the
  lanes Alt resizes too, so Alt+wheel resizes every track wherever the pointer
  is over the arrange view. Each modifier combination can be rebound.
  Saved `zoom_height` bindings are ignored on load, retaining current defaults
  and unrelated customizations without changing the mouse file schema.
- **Preferences** (Edit > Preferences…): app-wide settings, never project
  state. The default monitoring of new channels (off, on — the default — or
  auto; `preferences.json`, supple.preferences/1.6, sets
  `EngineApp::defaultMonitor`, so the agents' track.add follows it too), and
  a way into the mouse modifiers.
  **Audio processing threads** is an advanced live/background
  override: Automatic (default) or a limit per pool including its submitting
  thread; Limit 1 is serial. The menu
  offers 1, 2, 4, 8, 16, 32 and 64; `audio_threads` accepts any integer 1..64,
  with 0 meaning Automatic. Automatic uses the capacity derived from the CPUs
  the process may run on and
  graph-aware participation (engine.md §2.2, ADR-0023/0024). A note says it applies
  on the next engine start; changing it never restarts playback immediately.
  The limit applies independently to each pool, not total process threads;
  offline export keeps its separate thread setting. Version 1.0 files remain readable; missing `audio_threads`
  means 0, and saves write canonical 1.4. Invalid types/ranges return
  `preferences.audio_threads` at `/audio_threads` with a corrective hint;
  a failed save leaves the effective and displayed thread setting unchanged.
  **Lock process memory (mlockall)** is off by default, saved as the boolean
  `lock_memory`. Both 1.0 and 1.1 files remain readable; an absent field means
  false. Other types return `preferences.lock_memory`, `/lock_memory`, and
  "use true or false". The option applies on successful engine start or
  immediately when toggled while running. A failed save preserves the previous
  choice, following the existing audio-thread setter. A failed OS application
  keeps the saved/requested choice On with a failed effective note and errno
  plus a corrective hint. The row uses the same small grey left-column label
  and wide right-column On/Off button as the other preferences. Its two short
  caution lines explain RAM pressure, sample-library/stack failure, host-only
  scope and that turning it off also drops plug-in locks (engine.md, Optional
  host-process controls); a third says how to grant the permission it needs
  (an unlimited memlock limit for the audio group in
  `/etc/security/limits.d/`, and membership of that group). The status note appears only for pending intent
  ("Applies when the engine starts"), effective locking ("Locked") or failure;
  Off has no duplicate note or reserved space. Notes refresh on opening/toggling
  and model/service notifications; the dialog height follows visible
  content. Canonical saves
  preserve existing monitoring/thread values; no project format changes.
  **Keep CPUs out of deep idle** is separately off by
  default, persisted as boolean `prevent_deep_idle` in the same canonical
  file. Missing fields (including in 1.0/1.1 files) mean false; invalid
  types report `preferences.prevent_deep_idle` at `/prevent_deep_idle` with
  "use true or false". It uses the same save/intent/effective-state pattern
  as memory locking. The active note says "Requested: 20 µs"; failures show
  errno and a corrective hint; Off has no status note. This row uses the same
  label/button columns as the others. Its single small caution line says the
  request affects system idle while the engine runs and may increase power,
  heat and fan noise; a second says how to grant write access to
  `/dev/cpu_dma_latency` (a udev rule giving the audio group read/write, and
  membership of that group). Disable or
  engine stop releases only the owned request; transport stop retains it.
  Notes refresh through the existing GUI service flow. No preference test
  opens the actual latency device for writing.
  **Render folder** (1.3): the render dialog's folder choice (§4.8), an
  absolute path saved as the optional string `render_directory` and applied
  to `EngineApp::setRendersDir` at once and on every start. Absent (and in
  1.0–1.2 files) it means the default media folder; anything other than an
  absolute path reports `preferences.render_directory` at
  `/render_directory`. It is app-wide by design (ADR-0055): no command or
  project field sets it.
  **Open render folder when done** (1.6): the boolean `render_open_folder`
  (absent, and in older files, false; other types report
  `preferences.render_open_folder`), set from the render dialog's After
  render section.
  **Copy imported audio into project** (1.4): On by default, saved as the
  boolean `copy_imported_media` (absent, and in older files, means true;
  other types report `preferences.copy_imported_media`) and applied to
  `EngineApp::setCopyImportedMedia` at once. A note says the project then
  keeps working if the original file moves or is deleted.
  **Audio device** (1.5): the row shows the audio system, output device,
  rate and buffer, and opens the audio device dialog (also Edit > Audio
  device…, action `ui.audio.device`). Saved as the `audio` object:
  `backend` (`jack`, `portaudio` or `dummy`; absent: JACK on Linux, the
  native device elsewhere), `host_api` (absent: WASAPI where it lists
  devices, else PortAudio's first), `output_device`, `input_device`
  (exact names; absent: the host API's defaults), boolean `record`, `rate`
  (0: the device's or graph's own; up to 768000), integer `block` (16..4096)
  and boolean `force_graph`. Absent (and in older files) it means the
  defaults; a wrong value reports `preferences.audio` at `/audio/<field>`
  with a hint. The dialog's rows are menus: **Audio system** (JACK (PipeWire)
  on Linux; each PortAudio host API that lists devices — Core Audio, Windows
  WASAPI, ASIO — on macOS/Windows; and Dummy (no device)), **Output device**
  and **Input device** (native only: System default or an exact device; the
  input also None (playback only); ASIO uses the output device both ways),
  **Sample rate** (the device's or graph's own rate, or 44.1–192 kHz),
  **Buffer size** (32–2048 frames, with milliseconds) and, for JACK,
  **PipeWire graph** (Request, shared with other apps, or Force). A short note says what the choices mean on that system, and
  status lines show the running backend, rate, block, device period and
  output latency, a needed restart, or why the engine is not running.
  Restart audio and Stop audio buttons follow. Each choice is saved, then a
  running engine (or one whose start failed) restarts with it at once; a
  stopped one stays stopped. The native input opens all its channels up to
  32. At app start a refused saved choice retries the backend's defaults; the
  saved choice is kept. A failed start, or that fallback, opens the dialog,
  whose status says why (stderr is not seen from Finder or a desktop entry). engine.start receives the mapping
  (`engineStartArgs`): an explicit rate, or for the native device the output
  device's own rate; for JACK, `force_graph` only when forcing.



## 7. Visual identity

Steppe's own character, not a REAPER clone or a generic dark theme
(`tools/gui/mockups.py` renders mockups of it):

- **Material:** paper and ink — warm neutral grounds (light: off-white
  paper; dark: deep warm graphite, not blue-black), ink text, fine ruled lines
  for grids (the quire: gathered, ruled sheets). **Dark is the default.**
- **Two palettes, one accent each:** *warm* (an orange ink; the default) and
  *ink* (blue). The palette sets the accent, focus and selection; everything
  else is shared. View > switch light / dark and View > switch warm / ink
  palette apply at once and are saved in `<config>/<slug>/appearance.json`;
  `--theme` and `--palette` override them for one run.
- **Type-led hierarchy:** Inter for UI, IBM Plex Mono for every number and
  for notation; few font sizes; labels quiet, values prominent.
- **Strip types as bindings:** Channel blue, Buss amber, Master red, in every
  palette, on the TCP badge, the mixer strip's top edge and the routing
  matrix's marks. Masters carry a heavier spine on the routing marks; in the
  mixer they look like every other strip (the same thin top stripe) and sit
  pinned at its right edge. (Busses' double
  rule from the proposal is not drawn.)
- **Track colours, for organizing:** a small set (red, orange, yellow, green,
  teal, blue, violet, grey; `track.set {color}` also takes "#rrggbb") drawn as
  a stripe down the TCP row's left edge and under the mixer strip's name.
  Picked from the track's right-click menu (Rename, Color…, Duplicate,
  Delete; the same in the TCP and the mixer) or the `ui.track.color` action.
  A pick colours every selected track when the track is one of them (the
  action always works on the selection), as one undo step.
- **Signal kinds, for patching:** jacks and cables in the rack window are
  coloured by what they carry: audio amber, pitch blue, gate red, CV violet,
  unipolar green, phase teal (`signal-*` tokens, deepened in the light
  theme). Hues apart and weights alike, so no kind shouts, and none is a
  state colour. Module faces have their own tokens (`module`,
  `module-title`, `module-well`, `jack-hole`).
- **Calm by default:** colour reserved for state (armed, clipping, selected,
  agent-changed), meters and waveforms in ink tones; motion only for
  feedback. The TCP keeps its boxed toggles.

## 8. Commands and UI state

Every GUI action is a command or a UI action in the actions list
(`app_actions.h`); the GUI never edits the session any other way. A panel
keeps only the keys of a gesture in progress or a focused control (Escape
cancels a drag, Enter confirms a dialog, a list's arrows move its
selection); every other key is a keymap binding of an action, so it shows
in the actions list and can be rebound. State that
is not the session's (selection, zoom and scroll, panel sizes, screensets,
plug-in editor placements) lives in the UI sidecar (§1.2); `gui.selection`
exposes the selection to agents.

## 9. Performance and benchmarks

- `<slug> --bench gui` opens a generated 64-track session (audio items with
  peaks prebuilt, MIDI items, automation), runs scripted zoom/scroll/drag
  sequences offscreen and reports per-frame CPU time (p50/p95/max), UI-thread
  task durations over 2 ms (each bus pump with its sidecar and editor
  service, each panels' sync pass, each input event; frames are counted on
  their own), idle CPU over 10 s and cold start to first
  interactive frame. macOS reads the kernel process start timestamp through
  `proc_pidinfo`; unavailable timing still reports `-1`. Frame, CPU and idle
  measurements remain supported. Results in
  `docs/benchmarks/gui.md`; the gates of
  ADR-0005 §5 are acceptance criteria.
- Telemetry for long main-thread tasks in debug builds (a watchdog that
  logs any task over 2 ms with its command name).

## 10. Testing

`gui-app.tests` stages its agent skill tree, MCP helper and, when stock plugins
are enabled, eight stock modules covering the named FX/browser fixtures and
editor-capability checks as explicit build dependencies. Building this test
target alone supplies those prerequisites.

Unit tests for layout, input routing, damage, theme loading and the peak
pyramid; golden images for widgets and each view in fixed states (offscreen,
tolerance-aware); scripted UI tests that drive views by node id and assert
on the resulting session (through the bus) — the same commands the agent
uses, so a UI test can also be an agent workflow test. TSan for the
worker/UI handoffs. Plugin GUI embedding keeps the template's host probe
tests (`*-linux-host-probe`).

## Platform directory roots (2026-10-14, ADR-0035)

Configuration, data and peak-cache defaults share core-base's paths helper.
Linux retains its XDG/HOME locations. macOS uses Library/Application Support and
Library/Caches; Windows APPDATA and LOCALAPPDATA. User Documents/media defaults
use paths::home(). Product slugs and explicit file overrides remain caller-owned.
Windowing portability is specified in ADR-0039; native
plugin-editor embedding is integrated through ADR-0040. Editor integration tests
build on all platforms; actual editor display tests still require the exact
SUPPLE_GUI_DISPLAY_TEST=1 opt-in. Linux pipe/poll injection fixtures remain
Linux-only alongside portable waitable coverage.

The app accepts `--audio jack|portaudio|dummy|none`, a backend with its
defaults. Without it, interactive runs start the audio device saved in
Preferences (§6, **Audio device**): JACK on Linux and the native device on
macOS/Windows by default (ADR-0050); finite/offscreen runs default to none.
Tests use dummy/none and never enable hardware opt-ins.

SDL drivers are forced to X11/Cocoa/Windows respectively. Layout uses design
units; SDL pixel density and Windows per-monitor display scale determine the
framebuffer ratio. Pointer, file-drop and IME-caret coordinates use that same
mapping. PNG snapshots contain framebuffer pixels (design size times ratio).
Native `--offscreen` uses hidden desktop windows; Linux retains the EGL
offscreen driver. Offscreen and finite-frame runs close without an interactive
discard prompt, completing normal cleanup and any requested final snapshot.
Golden comparisons and reference updates are Linux-only;
native renders require GL and check content/size, with smoke images in CI.
macOS bulk rectangles align their edges to whole framebuffer pixels: Apple's
software rasterizer can otherwise cover half-pixel edges differently under
partial scissor clipping. Incremental/full repaint checks remain byte-exact.

**Native fixture coverage (2026-10-15):** socket-agent and screenset Python
integration tests run on Linux, macOS and Windows. The socket test uses the
existing portable AF_UNIX endpoint and bounded stdio pipe readiness. It saves
its private project before native window-close on Windows (TerminateProcess
cannot run shutdown/snapshot cleanup); POSIX retains SIGTERM. Final snapshots
must match the window's design aspect ratio at its framebuffer density. All
fixtures use dummy/no audio and private work/config directories.


### Distributed application startup

See `distribution.md` and ADR-0048. The Windows GUI uses a windowed subsystem
with SDL3's UTF-8 argument startup and parent-console output where available;
CLI redirection and offscreen snapshot flags remain supported. Other helper
programs remain console executables. The Apple Silicon app bundle declares
high DPI and microphone use and retains a build-tree command symlink. Fonts
load from the directory beside the installed skill before checkout fallback.
The application ID is the product slug, matching Linux desktop StartupWMClass.
Startup never depends on the launch directory: without a project or
`--workdir`, the engine's work directory (the base of relative command paths)
is the data folder `<data>/<slug>`. Finder starts apps in the read-only `/`,
which made the app exit at once; desktop and Start-menu launchers start in the
home or installation folder, which collected a stray `renders/`.
Linux/Windows windows use the embedded 256px application icon; Windows also
embeds the multi-size ICO resource for Explorer and Start-menu shortcuts.

### Track groups and VCA controls (ADR-0052)

The track context menu's **Groups…** submenu creates a named group from the
selected tracks and opens existing groups. A group menu edits its name, color,
linked attributes, membership and active state, and adds/removes its VCA.
Defaults link volume, mute and solo. Headers show small colored membership tabs;
the mixer adds named tabs that open the group menu. Suspended groups are dimmed
in headers and marked with a dot in the mixer.

Linked volume and pan gestures preserve relative offsets, including peers
outside the selection. A selected peer is changed once per gesture even when
several selected tracks share a group. A value reset applies the requested
value to the first selected member of each independent group (the touched
member first in its group), preserving that group's other offsets. Alt/Option held at the start of a fader
or pan gesture changes only that member. Group suspension disables links,
including selection and matched-span item edits, but leaves VCA sound active.

A group's optional VCA appears after audio strips and before pinned masters in
the existing mixer. It has a name/color, member count/menu, envelope button,
fader, mute and solo; it has no audio meter, pan, inserts, inputs or routing.
A right-click on its fader, mute or solo offers that control's MIDI learn
(§4.5, `vca.learn.set`, ADR-0058); elsewhere on the strip, the group's menu.
Members' fader handles continue to show their own levels. VCA volume/mute
lanes use the normal arrange automation editor and appear before the audio
track lanes. Nested VCA membership is available through typed commands.

Selection links select track peers together. Edit links select items on other
member tracks with matching project-sample start/end and the same lane token.
Move, trim, fade, mute, split, duplicate and remove reuse the typed linked
commands; batches with explicit peer operations bypass implicit propagation
so each target changes once. Take/MIDI-content edits stay local. Cross-track
moves require explicit destinations for every selected peer or command bypass.

### Frozen channels (ADR-0054)

Track-header and mixer context menus offer **Freeze** for channels and
**Unfreeze** while frozen. Freeze starts a background job through the command
bus; the existing job display supplies progress/cancellation. Headers and mixer
strips show a small snowflake (before the header's name, under the strip's name);
original items remain visible, dimmed with a small hatch and a snowflake before
their label. Existing fader/pan/mute/solo/send controls remain live. FX slots,
add-FX, polarity, mono, arm and monitor controls are disabled while frozen.
Frozen items can be selected; gestures cannot edit or move their content, and
opening an editor explains `track.unfreeze`. Commands enforce the same rule for
pooled edits and other callers. No additional panel or mode is introduced.

### Audio warp editing (ADR-0053)

Audio items display dim ticks for detected transients and solid handles/vertical
lines for authored stretch anchors. Waveforms use the same source-time map as
playback. Drag a hit to create an anchor, or drag an interior anchor to move its
item time; the source coordinate stays fixed. The overlay previews the destination,
current grid snapping applies (Shift frees it), and release commits one command.
A changed project/version cancels the marker gesture. Bundle anchors move together.

The menu bar's Item menu offers Detect hits, Add stretch marker at cursor,
Quantize audio, Slice at hits and Remove warping. Add stretch marker at cursor inserts an identity
anchor into each selected audio item whose interior contains the edit cursor,
using `take.stretch.add` without `source_pos`; existing anchors are not duplicated.
MIDI items are skipped. The entry is disabled unless some selected audio item
contains the cursor, is unlocked/unfrozen and nonlooped, and uses valid
Signalsmith pitch-preserving warp settings. It is one
undo step, independent of the menu pointer/grid, and preserves existing source
timing. Warp peers receive one edit per bundle; locks, frozen items and unsupported
loop/rate modes refuse the transaction as with other warp edits. Ordinary edit
groups do not propagate take-content changes.
Detect hits analyzes selected audio takes. Quantize audio uses the current arrange
grid and full strength; Slice at hits uses strength zero (split only). Both detect
missing or stale hits automatically, then apply the edit after the detection result
commits; this workflow has two undo steps. Explicit detection is still available
for inspecting candidates first. Changing the target items/takes while analysis
runs cancels the follow-up edit. There is no new panel or editing mode. The existing
status line displays detection, preparation and command errors.
