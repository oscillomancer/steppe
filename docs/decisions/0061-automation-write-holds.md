# ADR-0061: Automation writing holds the hand's level and pan

**Status:** Accepted, 2026-11-05.

Contracts: `specs/engine.md` §1.4, §1.6. Builds on ADR-0060.

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
  envelope plays from then on. A compile during a pass keeps the holds.
- Mute, plug-in parameters and the master playrate are not held and compile
  as before.

## Consequences

During a pass the engine follows the hand (after the render horizon) instead
of the envelope's older points, and a drag costs what ADR-0060 edits cost.
After a hold ends, playback follows what was stored. A touch held still for
more than half a second lets go early; an explicit release from the GUI's
gesture end would remove that limit. Offline renders are unchanged: holds
exist only on live plans.
