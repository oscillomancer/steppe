# ADR-0094: A file audition plays outside the project graph

**Status:** Accepted, 2026-11-18.

Contracts: `specs/engine.md` "File audition" and §6, `specs/core-analysis.md`
§9 (`readAudioHead`).

## Context

Nate wants to hear a file in the Browser before adding it. Nothing in the
engine played audio that is not an item; the only sound outside the
project's mix was the metronome. A file could be heard through the
project (a hidden track feeding the main master), or beside it.

## Decision

- **Beside the project, after its output.** The callback adds the file to
  the device's outputs 1 and 2 (mono on both, a wider file's first two
  channels) in `Engine::cycle`, after the transport's cycle and before LTC
  and the hardware MIDI lead's delay. It is heard in both executors,
  stopped, playing and counting in. It passes no track, master, plug-in or
  Console 9 stage and no meter; renders never run the callback and
  recordings take inputs and record taps, so neither can contain it. It
  does not follow a master's `hw_output` (ADR-0056), as the metronome does
  not. The other choice, feeding the main master, would meter it and apply
  the master's plug-ins, but would put a foreign signal into the project's
  graph and into anything recorded from the master.
- **Decoded before it sounds, off the model and audio threads, capped.**
  `audition.play` reads the file's header and answers; a worker reads its
  start (`readAudioHead`, which stops reading at the cap, so memory is
  bounded however long the file is) and resamples it to the device's rate;
  the model thread then publishes the voice. The cap is 28,800,000 frames,
  counted at the higher of the file's and the device's rates, so neither
  the decoded file nor the voice is longer: about 230 MB of stereo float at
  any rate, freed when the audition ends. That is ten minutes at 48 kHz and
  2.5 at 192 kHz. A cap in time would not bound memory: ten minutes at
  192 kHz is about 920 MB. Streaming from disk would remove the cap and the wait
  before a long file sounds; it is a larger piece of engine work.
- **Hand-over.** Each request (a voice, or a stop) is immutable and carries
  a serial; one atomic pointer publishes the newest. The callback reports
  the smallest serial it may still read (the request it took, the voice
  playing, the one fading out) and the model thread frees requests below
  it. `RtHandoff` does not fit: it returns the old object at adoption, and
  a fade still reads it.
- **No click.** A new voice or a stop fades what played over 5 ms; a voice
  starts on its first sample. A third request inside a fade cuts the older
  fade.
- **Transient.** Not an undo step and not saved. It ends at the file's end,
  on `audition.stop`, a new `audition.play`, the engine stopping (a device
  restart included) and a project replaced. No automatic play on selection.

## Consequences

- The project's real-time output equals its offline render as before: the
  audition is added after both are fixed, and only while one plays.
- A device with fewer than two outputs never runs the engine
  (`Engine::start` refuses it), so outputs 1/2 always exist; the callback
  still checks.
- Limits, in `STATUS.md`: outputs 1/2 only, the first 28,800,000 frames (ten
  minutes at 48 kHz, less at higher rates), from the start only, a wait
  before a long file sounds.
