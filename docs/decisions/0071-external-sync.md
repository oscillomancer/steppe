# ADR-0071: External sync input, LTC output and the project timecode

**Status:** Proposed, 2026-11-10. Builds on ADR-0070 (the sync library),
ADR-0045 and ADR-0046 (the master playrate) and ADR-0050 (machine-specific
preferences). Contracts: `docs/specs/session-model.md`, `docs/specs/engine.md`,
`docs/specs/gui.md`.

## Context

Following an external master (MIDI clock with Song Position, MIDI Time
Code, SMPTE LTC) and sending LTC need a project timecode, settings for the
machine's sync wiring, device input of the sync messages, an engine that
chases with the live playrate, and a way in from the GUI. This ADR records
those decisions; the first, the project timecode, lands alone so the clock
and MTC output work (phase 3) can use it.

## Decision

1. **The project has a timecode.** `project.timecode {rate, start}` (schema
   2.11): the frame rate (23.976, 24, 25, 29.97 drop-frame, 29.97
   non-drop, 30) and the label at project time 0. Timecode seconds are
   timeline seconds plus the start's real seconds (its frame count / fps,
   ADR-0070 §2), so a project starting at 01:00:00:00 plays 01:00:00:00 at
   its first beat. The default is **30 fps from 00:00:00:00**, and absent
   means the default, which is what older projects get: 30 is MTC's own
   code 3 and the usual default of other DAWs, an integer rate with no
   drop-frame labels, and from 00:00:00:00 timecode is project time, so
   nothing an old project shows moves. Each member is omitted at its
   default, so projects that never set it keep their bytes. It is project
   state (undoable, saved, part of branches) because it describes the
   material: picture and other machines agree on it per project, not per
   computer. `project.set {timecode: {rate?, start?}}` sets either member;
   a start the (new) rate does not have is refused with a hint. The GUI
   edits it in Project > Timecode….

2. **Sync settings are the machine's.** Which MIDI port or input channel
   the master is on, whether to follow its tempo, an offset, what a lost
   signal does and the LTC output belong to the computer's wiring, like the
   audio device (ADR-0050), not to the project: `sync.get` / `sync.set`
   (not undo steps, not saved in the project) and `preferences.json`
   1.7's `sync` object. Members: `source` (off, midi_clock, mtc, ltc),
   `midi_input` (a port's name, or part of it), `ltc_input` (1-based),
   `follow_tempo` (clock only; off follows Start, Continue, Stop and Song
   Position alone), `offset_ms` (positive plays behind the master),
   `on_loss` (stop, or freewheel for `freewheel_frames` timecode frames),
   `ltc_output` (off by default), `ltc_output_channel`, `ltc_level_db`. An
   LTC output channel the device lacks uses the device's last output, with
   a warning, and the setting is kept for a device that has it.
   `engine.status` and `sync.get` report what the engine hears: present,
   running, locked, the chase state, the master's tempo or timecode and
   rate, the chase rate, the error and the measured locate time.

3. **Devices deliver sync messages beside the channel messages.**
   `Backend::syncMidi()` gives each callback's Clock, Start, Continue, Stop,
   Song Position, MTC quarter frames and MTC full frames (at most 512 per
   callback, sorted by frame, with ports), up to ten bytes each, so a full
   frame travels whole. `midiInput()` keeps its channel-message contract.
   JACK classifies its events; the native buffer keeps per-port sync queues
   (512 each); CoreMIDI routes UMP type 1 (system real-time and common) and
   reassembles SysEx7 packets into the full frame; WinMM routes MIM_DATA's
   system messages and reads the full frame from its long buffers. Other
   system messages and SysEx stay counted drops. Everything is bounded and
   allocation-free on the producer and callback sides.

4. **The callback listens and chases; the main thread moves the
   transport.** A `SyncRuntime`, built on the main thread for a
   configuration (it allocates libltc's buffers) and adopted by the
   callback in publication order, runs after each cycle's transport work:
   it feeds the chosen port's messages to the clock or MTC receiver, or the
   chosen input channel to the LTC decoder (minus the device's input
   latency), runs ADR-0070's chase tracker against the position the
   listener hears (the processed timeline minus the plan's and the
   device's output latency), writes LTC output, and publishes a status
   under a sequence lock, waking the main thread when something it acts on
   changed. Locating needs source preparation off the callback, so
   `EngineApp::tick` does the transport: MIDI clock's Start and Continue
   play from their beat at the first clock, ahead by the measured time a
   seek takes plus the heard latency, in beats at the master's tempo (the
   project's until a second clock gives one: without follow tempo nothing
   would take a late start in afterwards); Stop pauses, Song Position while
   stopped locates; running timecode locates to timecode − project start −
   offset, ahead by the measured time a seek takes to be heard times the
   master's speed, and plays; the tracker's relocate requests locate again;
   a master running backwards, or a loss once final (at once, or after the
   freewheel time), pauses; MTC's full frame while stopped locates.

5. **The chase rate is a second scalar, never saved.** The live playrate is
   the saved `project.playrate.rate` times the chase scale. The scale lives
   in the engine only: no undo step, no project field,
   `transport.playrate.get`'s `requested_rate` stays the saved rate (its
   `effective_rate` shows the result). It is published like a knob move
   (ADR-0046): a scalar control on both executors' timelines, a ramp over
   1.5 times the audio played since the previous one (at least a device
   block) and the 150 ms low-pass, at most every 10 ms of audio. A saved-rate
   edit while slaved publishes saved × scale. Pitch is preserved.

6. **While following a source the engine runs a sync plan.** An input
   source compiles plans with every node in the live region (the callback
   and its live pool run the whole graph, as monitoring does for what it
   reaches), so transport changes land with the device's latency. Only when
   a chase can change the rate (MTC, LTC, MIDI clock with follow tempo) is
   the rate path on, with pitch-preserving stretch converters even at a
   rate of exactly 1, so the chase never needs a new plan. MIDI clock
   without follow tempo keeps the ordinary path and plays the same samples
   as without sync. LTC output alone needs no sync plan: it writes the
   timeline the callback plays (§9), and render-ahead stays. The stretcher
   at exactly 1 is not a null: a chasing session's audio clips differ from
   unsynced playback even when locked (in the tests a 441 Hz sine by about
   −25 dB of its level), which the tape path at unity avoids; MIDI and
   instruments are unaffected. Scalar edits on an all-live plan are placed
   after the callback's position, not after the 200 ms render-ahead. What latency remains: the
   main thread's wake (the GUI's tick, at most a frame; headless, the
   callback's wake), a seek's preparation (it compiles the plan: tens of
   milliseconds, measured and led), the device's output latency, and for
   the rate the ramp and the 150 ms low-pass, which the tracker's loop
   allows for (ADR-0070 §8). The cost: nothing renders ahead, so a heavy
   project has the callback's budget only, as when monitoring.

7. **Recording while slaved works.** Recording refuses a saved non-unity
   rate or rate envelope as before, but not the chase: the trajectory check
   skips sync plans. Takes are recorded in device time from the position
   where they start, so a take drifts from the timeline by (chase rate − 1)
   times its length: parts per million when locked to a master at its
   nominal speed. Following a different tempo by MIDI clock is for playback.

8. **Offline renders ignore sync.** They compile without the sync plan, so
   they stay bit-identical with sync on or off.

9. **LTC output.** With `ltc_output`, the callback writes LTC of the
   transport (the project's timecode, start applied) to the chosen device
   output channel after the masters are written, replacing what any master
   plays there (`sync.set` and `engine.status` warn when a master's
   `hw_output` includes it). **Timecode output is labelled with the audio
   it leaves with**: the masters' audio for timeline position t is
   processed at device frame d and leaves at d + the plan's latency, so the
   output written at frame d names the position processed at d − the
   plan's latency (the device's own latency then delays both alike). The
   callback keeps the timeline it processed as spans, split at loop wraps,
   for as long as the plan's latency, and reads them back that much later;
   after a start or locate the output is silent until the first processed
   frame is reached. Phase 3's MIDI clock and MTC output follow the same
   rule. LTC follows the playrate (each LTC byte ends where the timeline
   puts it, ADR-0070 §7), restarts at the next frame boundary after a
   locate or loop wrap (at the frame where the wrap is heard), and is
   silent at once when the transport stops.

10. **GUI.** Right-clicking the tempo button opens External sync: Off / MIDI
    clock / MTC / LTC (checked) and Sync settings…; while sync is on the
    button reads "EXT" and the tempo heard, tinted green when locked, in the
    accent colour while acquiring or chasing, red when no signal arrives.
    Sync settings… edits the members above and shows the status and the
    project timecode, with a way into Project > Timecode….

## Consequences

- A project can be slaved to a tape machine, a video deck, a drum machine or
  another DAW, and send LTC to them, with no change to what it saves.
- While following a source the whole graph runs in the callback.
- A chasing session plays its audio clips through the stretcher, which is
  close to but not the same as unsynced playback, even when locked.
- MIDI clock carries no absolute time: a clock dropout is bridged by
  counting from the last tempo (up to 2 s, ADR-0070 §4), and position is
  only as good as the count.
- Steppe does not play backwards: a master shuttling in reverse stops it.
- Real-hardware timing (MIDI interfaces, WinMM's millisecond timestamps,
  LTC over analogue lines) and native macOS and Windows behaviour are
  validated only by device trials; the tests use scripted devices.

## Alternatives considered

- **Chasing on the main thread.** Simpler to wire, but the tracker would
  compare positions sampled at GUI tick times; the callback has both
  timestamps exact.
- **A rate path of its own beside ADR-0046's.** A second converter control
  would duplicate the scalar path's two-executor scheduling; composing a
  scale into the existing scalar keeps one trajectory.
- **Keeping render-ahead while slaved.** Every transport and rate change
  would be heard 200 ms late, which the chase loop can survive but locates
  cannot.
- **Saving sync in the project.** Wiring differs between machines; a
  project opened elsewhere would listen to a port that is not there.
