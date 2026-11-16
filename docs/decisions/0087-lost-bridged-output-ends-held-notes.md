# ADR-0087: A bridged block's lost output ends the notes it held

**Status:** Accepted, 2026-11-16. Amends [ADR-0081](0081-live-bridged-plugins.md)
§4 (a late block is silent; its events are carried) for the plug-in's
output, and [ADR-0068](0068-plugin-midi-output.md) (the chain's MIDI bus).
Contracts: `docs/specs/engine.md` (the FX chain's MIDI bus, live deadline),
`host::OutputEvents::lost`.

## Context

Under ADR-0081 a bridged block that misses its live deadline is silent, and
later calls discard its output when it answers; the blocks skipped meanwhile
produce nothing either. ADR-0081 carries the *input* events so that no note
off reaches the plug-in late or never. Nothing did the same for its
*output*: a note plug-in (an arpeggiator, an echo) whose note off fell in a
lost block left that note sounding on everything downstream (later chain
stages, MIDI sends, hardware ports), and a pedal or bend it had set stayed
set. The plug-in itself believes it sent the off, so it never sends another.

## Decision

1. **The bridge says when a block's output is lost.** `host::OutputEvents`
   gains `lost`: the block returned silence and no events and its output will
   never arrive. It is set for a late block, a block skipped behind one, and
   a failed bridge (`RemoteInstance::process`). In-process plug-ins never set
   it.
2. **The host ends what that output held.** The chain's MIDI bus already
   keeps each note-output stage's held notes (`Stage::busHeld`, ADR-0068).
   On a lost block, `stageMidiBus` ends every one of them at the block's
   first frame, as the stage's own output, so every consumer of the bus hears
   them as it would the plug-in's.
3. **And resets the controllers it can know about.** For each of its first 8
   output ports the stage records which channels its MIDI output moved a
   controller on: any control change but the channel mode messages, pitch
   bend, poly or channel pressure. Reset All Controllers clears a channel's
   bit. A lost block sends Reset All Controllers on each recorded channel
   and clears the record. Ports from the 9th on are not tracked; CLAP note
   expressions are per note and end with their notes.
4. **In the lost block, not later.** The loss is certain when the block
   returns, so the ends go out at its start, one block earlier than waiting
   for the discarded answer. The plug-in's own later ends of those notes, if
   it sends any after answering, match no held note and pass no further.
5. **Bounded and real-time.** It walks the stage's fixed voice slots
   (`kBusVoices`) and a fixed 8×16-bit record, and emits through the bus's
   preallocated buffer, which counts overflow as it does for any output.

## Consequences

- A late bridged block can cut a note the plug-in meant to hold across it: it
  ends at the lost block and sounds again only at the plug-in's next note on.
  Cutting a note short beats leaving one hanging.
- The record and the release carry across plan swaps with the held notes
  (`carryBus`).
