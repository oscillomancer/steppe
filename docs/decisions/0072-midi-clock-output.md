# ADR-0072: MIDI clock, Song Position and MTC output

**Status:** Proposed, 2026-11-11. Builds on ADR-0069 (hardware MIDI
outputs), ADR-0070 (the sync library's generators) and ADR-0071 (external
sync, its §9 labelling rule and the project timecode). Contracts:
`docs/specs/engine.md`, `docs/specs/gui.md` (the preferences file).

## Context

Drum machines, sequencers, video and tape follow a DAW by MIDI beat clock
with Start, Continue, Stop and Song Position, or by MIDI Time Code. ADR-0069
gives the device layer (`Backend::writeMidi`, ports, a signed
audio-minus-MIDI latency per port); ADR-0070's `ClockGenerator` and
`MtcGenerator` make the messages for a mapping from frames to position.
What is left is which output sends what, where the messages are made, and
when they leave. A separate ADR (not folded into 0071): 0071 is about
following a master, this is about being one, and its settings are per
output port, which Phase 2's hardware MIDI routing extends.

## Decision

1. **Settings per output port, the machine's, in one record shared with
   hardware MIDI routing.** `MidiOutputSettings` `{port, enabled,
   offset_ms, reset_on_stop, clock, transport, spp, mtc,
   clock_while_stopped}`, keyed by the output's exact name. Phase 2's
   routing owns `enabled` (default true), `offset_ms` (signed, −1000..1000
   ms, positive later; one offset per port, shared by routed MIDI and
   clock) and `reset_on_stop` (default true); this ADR adds the clock
   members, all off by default, so a port sends no clock until asked. A
   disabled port sends nothing. `midi.outputs.set {port, ...}` changes one
   record (omitted members kept; a record back at every default is
   removed); `midi.outputs.get` returns `{ports: [...]}`: every output the
   running device has, then the stored records for outputs it lacks
   (`index` −1), each with the record's members and its state (§8). The
   GUI keeps the records in `preferences.json` as `midi_outputs`
   `{"<port>": {members}}` (supple.preferences/1.8) and saves every
   change, an agent's included, like the sync settings (ADR-0071 §2).

2. **The callback generates, in both executors.** The settings resolve on
   the main thread into `SyncConfig.outputs` (16, by
   `midiOutputPorts()` index: the kinds, and a delay of the port's
   `midiOutputLatency()` plus the offset in frames), part of the sync
   runtime (ADR-0071), which is built when any output sends. After the
   cycle's transport work the runtime remembers the timeline the callback
   processed (spans, split at loop wraps, shared with LTC output) and for
   each sending port runs a `ClockGenerator` and an `MtcGenerator` over the
   port's window, writing through `Backend::writeMidi` at
   `ceil(frame)`. Everything is fixed-size and preallocated (a 2048-event
   buffer, the span ring); `SUPPLE_RT`. Offline renders have no runtime and
   send nothing.

3. **Messages leave with their audio (ADR-0071 §9, extended).** The audio
   processed at device frame d leaves the device's outputs at d + the
   plan's latency; a MIDI event written at frame f leaves the port L
   frames before audio written at f is heard (ADR-0069's signed L). So the
   event for the position processed at d is written at
   **d + plan latency + L + offset**, and the window a port writes in a
   cycle `[c, c + n)` reads the timeline processed at
   `[c − delay, c + n − delay)`. With a positive delay that is history
   (spans, a ring of 4096: older reads as stopped). **A negative delay**
   (a MIDI port slower than the audio, or a negative offset beyond the plan
   latency) asks for what is not processed yet: the runtime predicts it
   from the newest span's speed, wrapping at the loop's end as the
   transport will. A start, stop or locate the prediction could not know
   lands with the first cycle that processes it, late by up to |delay|; a
   start there begins at the position the timeline is predicted to be at
   (Song Position of the next 16th and Continue, not Start).

4. **What a jump sends.** Each piece of the window is compared with where
   the previous one ended: a step over 5 ms of timeline is a locate
   (smaller ones, from a changed delay or a playrate ramp under
   prediction, the generators absorb). Start of play: Start at beat 0 (or
   before it: the plan's pre-roll, so Start precedes clock 0, which falls
   at beat 0's frame), else Song Position of the next 16th and Continue,
   the first clock at that 16th's frame (ADR-0070 §3). A locate or loop
   wrap while playing: Stop, Song Position, Continue at the frame where the
   new position is heard. Stop: Stop, then Song Position where the
   transport stands; a seek while stopped sends Song Position (and an MTC
   full frame). MTC: a full frame at start and at every jump, quarter
   frames from the next even frame boundary (ADR-0070 §4); quarter frames
   stop with the transport. The kinds filter what goes out: without `spp`,
   a start away from beat 0 is Continue alone (the device resumes where it
   was); without `transport` only clocks or positions go.

5. **Clock while stopped is a per-port option, off by default.** Most
   devices expect clock only while playing (they run their tempo display
   or arpeggiator from it otherwise); `clock_while_stopped` keeps F8 at the
   tempo where the transport stands, continuing the playing clock's phase.
   Start then restarts the grid at the 16th, as devices expect.

6. **Count-in.** Nothing is sent while the transport counts in: the
   timeline does not move, so the device starts with the song (Start or
   Continue at the first played frame, heard with its audio). With
   `clock_while_stopped` the free clock runs through the count-in, which
   lets a device lock its tempo first.

7. **No echo to the sync master.** When following MIDI clock or MTC
   (ADR-0071) on an input whose device is also a sending output (names
   equal without their direction words: capture/playback, in/out, input,
   output, MIDIIN/MIDIOUT), that output does not send back what the master
   sends (clock, transport, Song Position for a clock master; MTC for an
   MTC master), with a warning; the setting is kept and other kinds still
   go (MTC to a clock master's device, for instance). Nothing is refused.

8. **Status.** `midi.outputs.get`'s `ports` (and `engine.status.midi_outputs`,
   the same list): `{name, index, enabled, offset_ms, reset_on_stop,
   latency_ms, dropped, warnings, clock, transport, spp, mtc,
   clock_while_stopped, sends, suppressed, delay_ms, sent}`: the port's
   audio-minus-MIDI latency, what it sends now and what is suppressed, the
   total delay (plan latency + latency + offset), and the clock and MTC
   messages sent and refused (`writeMidi` false, or the event buffer full)
   since the engine started. Phase 2 adds its routed counts to `dropped`
   and `warnings`.

9. **No slave is left playing.** A runtime that leaves while a port's
   transport runs sends that port Stop first, at frame 0 of the callback
   (MTC sends nothing; its quarter frames just end), and nothing after:
   when its kinds change or the port is disabled, when the sync settings
   rebuild the runtime (the callback adopts the new one after the old one's
   Stop; a new runtime still sending transport then starts with Song
   Position and Continue), and when the engine stops. For an engine stop
   the main thread asks the callback for the Stop and waits (at most four
   periods or 50 ms) for a running cycle to send it, where every backend
   delivers MIDI, then the device's output latency (at most 250 ms) for it
   to leave, before stopping the backend. A device that runs no more cycles
   (it went away) gets it in the final zero-frame callback, which only the
   dummy backend takes MIDI in; JACK and native outputs refuse it there
   (counted as dropped), so a slave on a vanished device is not stopped.

## Consequences

- External devices follow Steppe sample-accurately at the device: clocks
  are where their beats are heard, across tempo changes, playrate and
  loop wraps, with plugin latency compensated, in both executors.
- A port with negative total delay is exact while the transport runs
  steadily, and late by up to the delay at a start, stop or locate. Users
  get there only with a negative offset larger than the plan latency plus
  the port's latency, or a MIDI port slower than the audio.
- During the plan's pre-roll before timeline 0, MTC names the timecode
  just before the project start (23:59:59:xx from 00:00:00:00), as LTC
  output does.
- The same-device test is by name; a false match suppresses only the
  echoed kinds and says so in the warning.
- Native CoreMIDI and WinMM scheduling (ADR-0069) and physical devices are
  untested; their jitter adds to what is generated here.

## Alternatives considered

- **Generate on a separate thread from the transport state.** Not
  sample-accurate, and it would need the plan's latency and the device's
  timing anyway; the callback knows both.
- **Clamp negative delays to zero.** Simpler, but every message would then
  be early by the remainder for the whole session; predicting keeps the
  steady state exact.
- **Fold into ADR-0071.** Its settings are the machine's sync input; these
  are per output port and grow with Phase 2's routing.
