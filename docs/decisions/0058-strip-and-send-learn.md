# ADR-0058: MIDI learn on strips' and sends' own controls

**Status:** Accepted, 2026-11-04. Contracts: session-model.md §1.4, §1.5, §5,
§7, engine.md (control messages), gui.md §4.2, §4.5, §4.6 and §6. Builds on
ADR-0027 (MIDI learn), which left "learn on track faders directly" undone.

## Context

ADR-0027 binds controllers to plug-in parameters (project state) and to
actions (user settings). A track's fader and pan followed a controller only
through "Track: volume of selected tracks (MIDI CC)": one knob for whatever is
selected, so a controller with eight faders could not drive eight tracks.
Buttons (mute, solo, arm) could only run actions, which act on the selection
too, and sends had no controller path at all.

## Decision

1. **Bindings live on the track or the send.** `Track::learn` and
   `Route::learn` hold one binding per control: `{param, midi, mode,
   soft_takeover}` (the `midi` and `mode` of ADR-0027). A track's controls are
   `volume`, `pan`, `mute`, `solo`, `phase_invert`, `mono`, `armed`,
   `monitor` and `lane_mute`; a send's are `volume`, `pan`, `mute`,
   `phase_invert`, `mono` and `tap:post_fader|pre_fader|pre_fx`. They are
   saved with the project (`supple.project/2.6`), undoable, and copied with
   a duplicated track, as its effects' bindings are; a track template keeps
   only the strip fields it lists, not these. They go when their track or
   send goes. `track.learn.set / remove / list` and
   `route.learn.set / remove / list` edit and read them; the set commands
   refuse a control the strip lacks (`track.strip_type`) and toggle mode on a
   level or pan (`learn.mode`).
2. **The engine moves them as edits.** `EngineApp::pollControl` applies them
   after the plug-in bindings, through `moveControl`: a `track.set` or
   `route.set` with a gesture per binding, so a movement is one undo step,
   group links follow as they do for the fader, and touch/latch/write
   envelopes record it. A level follows the fader's travel (the shared
   `supple::fader` taper, so a knob and the on-screen fader agree); a pan's
   centre detent is the centre exactly; an encoder step is 1/128 of the
   travel or 1 % of pan.
3. **Buttons are switches.** A note or program change flips the switch on
   each press, as a CC in toggle mode does; an absolute CC is on in its upper
   half and off below (a momentary button holds it); an encoder turns it on
   stepping up and off stepping down. `monitor` steps off → on → auto on each
   press; a tap control selects its tap. A frozen track's bindings move only
   what a frozen strip still allows (volume, pan, mute, solo).
4. **The GUI learns on a right-click.** A right-click on a fader, pan or
   button of a TCP row or a mixer strip, or on a control of the routing
   matrix, offers "MIDI learn: <control>…" (naming the bound control), which
   opens ADR-0027's dialog: soft takeover for a level or pan, toggle as the
   default CC mode for a button. The matrix binds every selected send, in one
   undo step, as its other controls act on all of them.
5. **"Routing: last touched send (MIDI CC)"** follows a knob on the send
   whose level or pan the routing matrix moved last, through `moveControl`,
   with soft takeover per send and control — as "FX: last touched parameter"
   does for plug-ins.

## Consequences

- Each strip and send can have its own controller, saved with the project;
  agents map and read them with the same commands.
- Learned moves are ordinary edits: undo, automation, group links and the
  strip rules apply without a second path.
- A knob bound on several sends at once (a matrix selection) sets each of
  them to its position; it does not keep their offsets as a drag does.
- Not done: VCA faders (`vca.set`) and the matrix's channels menu take no
  bindings.

## Alternatives considered

Per-track "(MIDI CC)" actions (one action per track and control: the actions
list would grow with the project, and action bindings are user settings, not
the project's); bindings in the per-project UI file (not undoable, invisible
to agents, lost with templates); one project-level binding list (the bindings
would outlive the tracks and sends they name, and duplicates would not carry
them).
