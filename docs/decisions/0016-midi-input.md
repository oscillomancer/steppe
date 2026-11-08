# ADR-0016: MIDI input — monitoring and recording

**Status:** Accepted, 2026-09-27. Spec: `docs/specs/engine.md` §4 ("MIDI
recording into MIDI takes"), `docs/specs/session-model.md` (routes from
`midi_input`, `record.mode: midi_overdub|midi_replace`, route `midi`
channel mapping). Builds on ADR-0004 (JACK), ADR-0014 (recording, the live
region), ADR-0015 (loop passes).

**2026-10-15 amendment:** [ADR-0042](0042-native-midi-input.md) extends the
backend input contract to CoreMIDI/WinMM through PortAudio on macOS/Windows.

**2026-11-07 amendment:** [ADR-0065](0065-independent-record-inputs.md)
lets a rack's channel keep a MIDI input beside its Out, fixing the rack
home-channel monitoring contract; hardware audio and MIDI still replace
each other. The older single-input representation still loads, and
`record.take` says what such a channel records; see the current
[rack](../specs/rack.md) and [session-model](../specs/session-model.md)
contracts.

## Context

The model can already say "this track records from MIDI device X"
(`record.input` → a route whose `from.midi_input` names a device, with an
optional `midi {from_channel, to_channel}` filter). The engine has no MIDI
input: backends deliver audio only, so an armed instrument track can
neither be played live nor record.

## Decision

1. **Backends deliver MIDI beside audio, without changing the callback.**
   `Backend::midiInput()` returns the current cycle's events (valid only
   inside the callback): `{frame, port, bytes[3]}`, sorted by frame, at
   most 1024 per cycle (more are counted as dropped). `BackendConfig::
   midiInputs` asks for MIDI; `Backend::midiPorts()` names the ports.
   - **JACK:** one input port per physical MIDI capture port (at most 16),
     connected at start and named after it; the device name in a route
     matches a port's name or alias by case-insensitive substring
     (`"*"` or empty: every port). System-exclusive and other long
     messages are skipped (counted).
   - **Dummy:** a test generator fills the cycle's events.
2. **Live MIDI is the live region.** A track is live when it monitors a
   MIDI input route, exactly as for audio (ADR-0014): `monitor: on`, or
   `auto` while armed. Its device events, filtered and re-channelled by the
   route's `midi` mapping, join the node's block events at its note target
   (after timeline MIDI at the same frame), so the instrument plays in the
   callback at the device block. Held live notes get note-offs when
   monitoring stops or the transport relocates.
3. **Recording** captures the filtered events of each armed MIDI track
   into a per-track SPSC ring (4096 events; overflow counted) on the audio
   thread, stamped with the timeline sample of the cycle position they were
   heard at (MIDI arrives with the device's input latency only: the event
   frame within the cycle, minus the capture latency, placed like audio).
   Loop wraps start a new run (ADR-0015). No writer thread: events are
   small; the main thread drains the rings when the recording stops.
4. **Commit** builds `mi_*` sources: notes paired from note-on/off (a note
   still held at the stop ends there), CC/pitch bend/aftertouch as
   controls, in the project's PPQ via the tempo map. Takes and items follow
   the audio rules: one item per run, or one item on the loop with a take
   per pass, clipped to the punch range. `midi_replace` and `input` create
   new items; `midi_overdub` merges the notes into the MIDI item already
   under the recording's start on that track, as a new take of it (the old
   take stays, so undo and take switching both work).
5. **Crash recovery** for MIDI is out of scope at first (events live in
   memory until stop); a recording longer than the ring holds is not lost —
   the main thread drains the rings every service() call while recording.

## Consequences

- An LLM can arm a track on a MIDI keyboard and record a performance, then
  read it back as notation (`midi.get {format: notation}`).
- The Backend interface grows two virtuals; the bench's paced backend and
  the test backends return no MIDI.

## Alternatives considered

MIDI through the audio callback's signature (every backend and test
double changes for a feature most ignore); a MIDI thread outside the audio
callback (timing would be the thread's, not the device cycle's, so events
could not be placed sample-accurately); one merged input port for all
devices (loses which device an event came from, so routes could not
select a device).

## Implementation notes (2026-09-27)

- `MidiInputEvent`, `Backend::midiInput()/midiPorts()`, `BackendConfig::
  midiInputs`, `routeMidi()` (port mask + channel filter/remap, channel
  messages only) and `midiPortMask()` in realtime.h. `DummyBackend::Options
  ::midiPorts/midi` (tests). JACK: the MIDI symbols are optional (a libjack
  without them simply has no MIDI); one `midi_in_N` port per physical MIDI
  capture port (≤16), connected at start; each period's events are
  collected, sorted, and handed to each engine block rebased to it; long
  messages and overflow are counted. The engine takes MIDI through a
  pointer set before the backend starts (not `backend_`, which is set after
  `start()` returns — the first callback can come earlier); JACK publishes
  its state to `midiInput()` before activation for the same reason.
- Plans: `Node::midiIn` (device, channel map; ports resolved by
  `Engine::load` from the backend's port names) makes a node live; device
  events for the part of the cycle being processed (`deviceMidi`,
  `deviceMidiOffset`) join the note target's block events after timeline
  MIDI, playing or stopped. `Node::liveHeld` counts device notes;
  `carryLiveNotes` hands them to the next plan's node at a swap (released
  there if it no longer plays MIDI live); an instance leaving an executor
  is `reset()` before it stops (the anticipative hand-over to the renderer
  when monitoring stops). Loop wraps do not cut device notes.
- Recording: MIDI targets have no file; the audio thread stamps each
  routed event with its captured-frame position into a per-track SPSC ring
  (4096; overflow counted); the recorder's writer thread drains the rings
  every 20 ms (and finish() after joining it), so a long performance does
  not depend on the main thread. `finish()` places each event with
  the recording's runs (the same latency-shifted run table as audio), so
  an event played just before a loop wrap stays in its pass.
- Commit: runs/passes/punch as for audio; each take gets a new `mi_*`
  source — notes paired per channel and key (a retrigger ends the previous
  one; still held at the run's end: ended there), other channel messages
  as controls, in the project's PPQ from the item start. `lost_frames`
  counts lost events for MIDI.
- Bus: `track.set {record_midi: device, midi_channel}` makes the record
  input a `midi_input` route (audio `record_input` replaces it and vice
  versa); `transport.record` records audio and MIDI tracks together.
- Tests: JACK MIDI from two devices reaches the right block at the right
  frame (sysex dropped); a keyboard plays a monitored instrument
  sample-accurately (filtered by channel and device), stopped or playing,
  in both modes; MIDI events are placed per loop pass; monitoring off with
  a key held releases the note; over the bus, a MIDI performance commits as
  notes one beat apart and reads back as notation.
- `midi_overdub` (`track.set {record_mode}`): the recording merges into
  the MIDI item under its first heard position as a new take (a copy of
  the active take's notes plus every pass's recorded notes, in the take's
  PPQ and start offset); with no item there, new items as usual.
- Not yet: crash recovery of MIDI, MIDI output ports.
