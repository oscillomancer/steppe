# ADR-0068: Plug-in note and MIDI output

**Status:** Proposed, 2026-11-10 (draft; the number is assigned at merge).
Amends ADR-0013 (the bridge's data plane) and ADR-0017 (VST3 event buses).
Contracts: `docs/specs/sandbox.md` §1 and its implementation notes, and
`host/clap-host/include/supple/host/clap_host.h`.

## Context

The host library delivered notes and MIDI to plug-ins but dropped what they
sent back: `OutputEvent` carried parameter values, gestures and note ends
only. Arpeggiators, chord generators, MIDI effects and sequencer plug-ins are
useless without their output, and the MIDI sends work introduces a per-track
post-FX MIDI stream that plug-in output must be able to feed. The engine
consumes events from three isolations (in process, a shared bridge, a bridge
of its own), and offline renders are deterministic and equal to real time,
so the events must come out the same in all three.

## Decision

1. **Four new output kinds, appended.** `OutputEvent::Kind` gains `NoteOn`,
   `NoteOff`, `NoteChoke` and `Midi`, after the existing four so their
   numbers do not change. Notes keep the plug-in's output note address
   `(port, channel, key, noteId)` and their velocity 0..1 in `value`; `Midi`
   holds one MIDI 1.0 short message in a new `midi[3]` field (status first,
   unused data bytes zero) and its `port`. The field sits in the padding
   after `kind`, so the struct stays 40 bytes and trivially copyable.
   `midiOutput()` names the four kinds. Notes that arrive as notes stay
   notes, and MIDI stays MIDI: the host does not rewrite one into the other,
   so a consumer sees what the plug-in chose to send.
2. **What maps to MIDI 1.0, and what is counted.** CLAP `NOTE_ON/OFF/CHOKE`
   are notes; `CLAP_EVENT_MIDI` short messages (channel voice, system common,
   real-time) are `Midi`. `CLAP_EVENT_MIDI2` maps when a single short message
   expresses it: UMP message types 1 and 2 exactly, type 4 by the MIDI 2.0
   down-conversion (note on/off with velocity 16 → 7 bits and a note on never
   at 0, poly and channel pressure, CC 32 → 7 bits, pitch bend 32 → 14 bits,
   a program change without bank). A pressure note expression on one key
   (`CLAP_NOTE_EXPRESSION_PRESSURE`, concrete channel and key) is poly
   pressure. VST3 note on/off are notes, poly pressure events and legacy MIDI
   CC output events (CC, channel pressure, pitch bend, program change, poly
   pressure, quarter frame, song select/position, tune request,
   start/continue/stop, active sensing) are `Midi`. Everything else is
   dropped and counted in the sink's new `OutputEvents::unsupported`: SysEx
   (CLAP `MIDI_SYSEX`, VST3 data events; out of scope), the other note
   expressions (tuning, volume, pan, vibrato, expression, brightness, and all
   VST3 note expressions), other MIDI 2.0 messages (per-note controllers and
   pitch bend, RPN/NRPN, a program change with bank, data messages), VST3
   chord/scale events and cable select, and malformed events (a data byte
   with its top bit set, a note on without a concrete channel and key, a
   non-finite velocity).
3. **Times and order.** Note and MIDI output times are clamped to the block
   (0..frames−1). Each `process()` call sorts the events it appended by time
   with a stable insertion sort (no allocation, linear for already-ordered
   output), for every backing, so same-time events keep the plug-in's push
   order. Parameter outputs are sorted with them; their times are not
   clamped, as before.
4. **Overflow is the sink's.** Events beyond the caller's preallocated
   storage are refused (CLAP `try_push` returns false) and counted in
   `dropped`, as before. A VST3's own output list holds 2048 events; events
   it refuses are counted in `dropped` too.
5. **Identical across isolations.** The bridge runs the same conversion in
   the child, then copies the events through the shared-memory output arena
   (`kMaxOutEvents` = 1024). The Header now carries whether the host passed a
   sink and the room it has left (at most 1024), and the child gives the
   plug-in a sink of exactly that room: the plug-in sees the same pushes
   accepted and refused as in process, so events, `dropped` and
   `unsupported` are bit-identical in all three isolations whenever the room
   is at most 1024. A count above the room, or an event kind past `Midi`, is
   a protocol violation and fails the bridge. The Create reply says whether
   the plug-in outputs notes. Protocol version POSIX 20, Windows 21.
6. **Declared note output.** `Instance::outputsNotes()` is true for a CLAP
   plug-in with an output note port (`note-ports`, `is_input = false`) and
   a VST3 with an event output bus, which the host now activates.
7. **Flush and the main thread.** `params.flush` output belongs to no block:
   note and MIDI output there is dropped and counted in the idle drain's
   `dropped`. The engine does not publish note or MIDI output to the
   main-thread parameter-output queue; it stays on the audio path for the
   FX chain's MIDI bus (the next stage of this work).

## Consequences

- Plug-in note and MIDI output is available to the engine's plan with no
  allocation on the audio thread; until the engine consumes it, a stage's
  output sink (256 events) is shared between parameter and MIDI output.
- A host sink larger than 1024 events behaves differently bridged (the arena
  caps it); no caller uses one.
- MIDI 2.0 resolution and per-note expressions beyond pressure are lost.
  SysEx output needs a variable-length arena and is left for later.
