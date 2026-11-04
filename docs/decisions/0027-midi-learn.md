# ADR-0027: MIDI learn — controllers drive parameters and actions

**Status:** Accepted, 2026-10-08. Specs: `docs/specs/session-model.md` §1.7
(fx `learn`), `docs/specs/engine.md` (control messages), `docs/specs/gui.md`
§4.5 and §6. Builds on ADR-0016 (MIDI input) and ADR-0007 (every change is
a command).

**Amendment, 2026-10-08:** [ADR-0030](0030-main-thread-scheduling.md) replaces
the polling schedule described below with notifications and monotonic service
deadlines. The GUI loop conversion follows separately.

**Amendment, 2026-11-04:** [ADR-0058](0058-strip-and-send-learn.md) adds
bindings on tracks' and sends' own controls (fader, pan, buttons), the
"learn on track faders directly" left undone below.

## Context

A MIDI controller's knobs, faders and buttons should be able to move any
plug-in parameter and run any action, learned by moving the control, the
way REAPER does it (its "Learn" on a parameter, and MIDI shortcuts in its
actions list): a CC mode for encoders (three relative conventions) and
buttons (toggle), soft takeover, and conditions ("only while the track is
selected", "only while the effect's window has the focus" or "shows").
Device MIDI reaches the audio callback only (ADR-0016); the model changes
only through commands on the model thread.

## Decision

1. **Control messages leave the callback through a ring.** Each callback
   copies its device events that are notes, CCs, program changes or pitch
   bends (`ControlMidiEvent {port, bytes}`) into an SPSC ring (1024; more
   are counted as dropped). Every device is heard; there is no separate
   "control" device setting. `Engine::takeControlMidi` drains it on the main
   thread.
2. **Plug-in parameter bindings are project state.** `FxInstance::learn`
   holds one binding per parameter: `{param, midi: {kind: cc|note|
   pitch_bend|program, channel, number, device}, mode: absolute|relative1|
   relative2|relative3|toggle, soft_takeover, when_selected, when_focused,
   when_visible}`. They are saved with the project, travel with FX chain
   presets, templates and copies, are undoable, and an agent edits them with
   `fx.learn.set / remove / list`. `device` is matched like a MIDI route's
   (a case-insensitive substring; empty: any input) — port names carry USB
   paths, so the dialog's default is any input.
3. **`EngineApp::pollControl` moves them.** It drains the ring, hands the
   messages to `onControl` (the window app), and — unless a learn dialog is
   listening — applies each heard binding: every message in order (relative
   steps add up, the last absolute position wins), quantized for stepped
   parameters. A semantic parameter goes through `fx.param.set` with a
   gesture per binding, so a movement is one undo step and touch/latch
   envelopes record it. A plain (CLAP/VST3) parameter is set on the running
   instance at once and committed when the control rests (300 ms), the way
   an edit in the plug-in's own editor is (`commitOutputGesture`); without
   a running instance it goes through `fx.param.set`. `tick()` calls it (100
   ms); the window app adds a 10 ms timer while anything is bound or a
   dialog listens.
4. **`when_*` conditions are the window app's.** `EngineApp::learnGate`
   answers them from the selection and the windows; without one (headless)
   a conditioned binding waits.
5. **Action bindings are user settings.** `controls.json`
   (`supple.controls/1.0`, beside the keymap) maps a control to an action.
   A press (note on, CC above 0, program change, an encoder step) runs the
   action once; actions that follow a value ("… (MIDI CC)": selected
   tracks' volume and pan, the master's volume, a beat jog) receive the
   position or the steps instead, one undo step per movement.
6. **One dialog learns both.** It shows the control moved last, its CC mode,
   whether to listen to that input only, and for a parameter the
   conditions and soft takeover; OK binds, Remove unbinds.

## Consequences

- An LLM can map a controller (`fx.learn.set`) and see what is mapped
  (`fx.learn.list`), the same bindings the user makes by moving a knob.
- The audio thread does one bounded copy per callback; the main thread
  wakes at 100 Hz only while something is bound.
- A semantic parameter answers within a poll plus a model version; a plain
  one within a poll, its project state following at rest.
- Not done: OSC; "arm envelope for the parameter"; learn on track faders
  directly (the "(MIDI CC)" actions cover the selected tracks).

## Alternatives considered

Applying learned values on the audio thread (REAPER-like latency, but the
model would no longer be the truth and undo, automation recording and agents
would not see the moves); bindings in the per-project UI file (not undoable,
lost with FX chain presets, invisible to agents); a "control" flag per device
(PipeWire port names change with the USB port; matching by substring and an
"any input" default needs no setup).

## Amendment (2026-10-08): the last touched parameter

`EngineApp` remembers the parameter edited last in the window — an
`fx.param.set` from the window (`Origin::Ui`; a plain parameter named by
name is remembered by its id), a gesture begun in a plug-in's own editor,
or an editor edit committed from its outputs — but not agents' edits nor
what learned controls move. "FX: last touched parameter (MIDI CC)" moves it
through `EngineApp::moveParam`, the same step learned bindings use (one undo
step per movement, a plain parameter live at once and committed at rest).
An absolute knob takes over softly, per parameter, so switching targets
never jumps the new one; an encoder moves it at once. The FX window's tab
row names it ("Last touched: Plumb · Gain"). A plug-in editor that reports
no gestures or outputs is not noticed.
