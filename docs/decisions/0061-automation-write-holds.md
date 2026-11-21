# ADR-0061: Automation writing holds the hand's level and pan

**Status:** Accepted, 2026-11-05.

Contracts: `specs/engine.md` §1.4, §1.6. Builds on ADR-0060.

**Amendment, 2026-11-20:** plug-in parameters are held too, from the
plug-in's own gesture; see the amendment at the end.

## Context

Automation writing (P5.14) records a strip's volume, pan and mute, a send's
volume, pan and mute, and a VCA's volume and mute into envelopes in touch,
latch, latch_preview and write mode while the transport plays. Each move
added points and the engine compiled the envelope again. What was heard was
the curve at the render position: points written at the heard position sit
behind the renderer, so it played the envelope's older points ahead of them
rather than the control, and every move paid for a compile (with rate
scheduling, a prepared rate plan too). Mixing with touch or latch was where
fader lag hurt most.

## Decision

While a pass writes a volume or pan target (a strip's or send's level or
pan, a VCA's level), the engine holds it: the node, the edge, the source
copies sends keep of their strip's curves, and the VCA control play the
target's static value, which the same edit set to the control's position,
instead of the compiled curve.

- `EngineApp` derives the holds from the pass's written envelopes and hands
  them to `Engine::setWriteHolds` before each load. A hold is a flag in the
  ADR-0060 mix slots, so starting one compiles nothing.
- A session whose other differences are point changes in held envelopes (in
  a writing mode, nothing else on the envelope changed, and a send
  envelope's presence unchanged) takes the ADR-0060 path. The plan's curve
  for that target is then stale, which is harmless while it is held.
- Latch, latch_preview and write hold until the pass ends at pause or stop,
  matching the value they store to the stop. Touch holds until 500 ms after
  its last write, on `EngineApp`'s service clock with a service deadline.
  Nothing reports a strip control's release yet; a plug-in's own gesture end
  still releases its touch envelope.
- Ending a hold whose curve is stale compiles the session, so the written
  envelope plays from then on. A compile during a pass keeps the holds. Every
  load path carries hold changes, the playrate scalar's shortcut included.
- Holds exist only while the transport plays. Undo, redo and a branch switch
  mid-pass start the pass again where it is: the writes so far belong to the
  version left behind, so they are no longer held and get no latch tail at
  stop. A halt ends the pass that was playing: its successor, whether it
  plays or records, inherits no holds and writes no latch tail from it.
- Mute, plug-in parameters and the master playrate are not held and compile
  as before.

## Consequences

During a pass the engine follows the hand (after the render horizon) instead
of the envelope's older points, and a drag costs what ADR-0060 edits cost.
After a hold ends, playback follows what was stored. A touch held still for
more than half a second lets go early; an explicit release from the GUI's
gesture end would remove that limit. Offline renders are unchanged: holds
exist only on live plans.

## Amendment (2026-11-20): plug-in parameters are held

**Context.** A plug-in parameter's lane kept sending its curve during a
write pass. The editor's knob and the lane fought: the plug-in played the
envelope's older points over the hand. With modulation through the envelope
(ADR-0096 step M4), the lane sent the curve plus the modulation, and the
commit took one offset off every recorded point: the offset as it stood at
the gesture's end. So each stored point was off by however far the source
had moved since. Review 63 asked about this. The co-director chose to hold
plug-in parameters as volume and pan are held, over correcting the offset
point by point.

**Decision.** In a write pass, a plug-in parameter's lane sends nothing while
it is held. The plug-in keeps what the hand set.

- **The engine holds it from the plug-in's own gesture.**
  - A lane whose envelope is in a writing mode, on a real-time plan, holds
    from the block after the plug-in reports the gesture's begin while
    playing.
  - Touch holds to the block in which the gesture ends. Latch, latch_preview
    and write hold to the pass's end.
  - Main-thread holds would start one service round, or for the renderer a
    render-ahead, too late. A moving lane sends every 32 samples, so it would
    take the knob back until then.
  - A plan swap carries an open gesture, and a latch the main thread has not
    let go of (by CLAP id, with the lane's other memo).
  - A transport start or seek ends the engine's own holds. The renderer
    never runs a stage while stopped, so it cannot see a stop there.
- **`EngineApp` holds it too, as it holds a level.** A touched envelope in a
  writing mode is handed to `Engine::setWriteHolds`, a flag per lane in the
  mix slots. This covers:
  - a plug-in that reports values without a gesture;
  - the latch after the gesture's commit, until the pass ends.

  When this hold goes (the pass ends, or an undo, redo or branch switch
  starts it again), the engine's latch goes with it.
- **The hand's values are recorded as they are.** A gesture the engine held,
  or a value while `EngineApp` holds the target, gets no offset taken off.
  The model's static value is the hand's last.
- **Released, the lane sends its value at once,** then on its grid. From the
  next grid point (at most 32 samples) the output is the offline render's
  again.
- **Unchanged:**
  - A plug-in parameter's written points still compile (they change the FX's
    values too, which ADR-0060 does not carry).
  - Mute and the master playrate are still not held.
  - Offline renders have no hand, and their plans hold nothing.

**Consequences.**
- During a touch the plug-in plays the hand, whatever the curve or the
  modulation does.
- Modulation through an envelope pauses on that parameter while it is held.
  It adds to the recorded points again from the release. A route that
  modulates the plug-in directly (CLAP parameter modulation) goes on, as it
  never moved the parameter's value.
- The first block of a gesture is not held: the lane's events for it went
  out before the plug-in reported the grab.
- If an undo comes before `EngineApp`'s hold of a latch has reached the
  engine, the engine's latch lasts to the pass's end. That window is the gap
  between the gesture's end and its commit's plan.
