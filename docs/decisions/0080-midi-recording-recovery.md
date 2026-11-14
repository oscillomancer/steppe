# ADR-0080: MIDI takes survive a crash through a journal

**Status:** Accepted, 2026-11-14. Extends ADR-0014 (recording and its crash
recovery), ADR-0016 (MIDI recording) and ADR-0078 (unsaved projects come
back after a crash). Contracts: `docs/specs/engine.md` §4 (Recording),
`docs/specs/session-model.md` §5.3, the `project.recoverable` and
`project.recover` commands.

## Context

An audio take survives a crash: its WAV and sidecar are on disk from the
first block, and `project.recover` places what a crash left out. A MIDI take
lived only in memory until stop committed it to the project (ADR-0016), so a
crash lost the whole take, and with it often the best performance of a
session (STATUS listed this as a known limit). The new recovery popup
(ADR-0078) shows a project's and a left-behind session's recordings; MIDI
takes should be among them.

## Decision

1. **A journal per MIDI take.** Recording gives each MIDI target a file
   beside the WAVs, `<media>/rec/<Track>-<n>.midi.jsonl` (the same free
   `<Track>-<n>` stem rule; created exclusively, never through a link). It is
   JSON Lines, schema `supple.midi-recording/1.0`, one compact JSON value per
   line:
   - a header object: `track`, `sample_rate`, `start_sample` (the timeline
     sample of captured frame 0, the device round trip already taken off),
     `latency` (that round trip), `device` (device ports were recorded),
     `routed` and `shift` (Note Out input and its clock), `loop` (the loop in
     effect at the start, `[0, 0]` when none);
   - an event: `[at, status, data1, data2]`, plus `route` and `voice` for a
     Note Out voice, `at` counted in captured frames as an audio file's
     frames are;
   - `{"run": [captured frame, timeline sample]}` for each loop wrap or seek;
   - `{"captured": n}`: how far capture got.
2. **Written off the audio thread.** The audio thread does what it did: it
   pushes events into the per-track SPSC rings and, new, publishes how far
   capture got with one atomic store. The recorder's writer thread, which
   already drains those rings every 20 ms, appends what it drained to the
   journals, flushes them to the OS on every pass and fsyncs them every 5 s.
   A journal that cannot be made or written does not stop the recording: the
   take is committed at stop as before, with a `record.journal` warning.
3. **The loss window.** If the program crashes, the take is kept up to the
   writer's last pass: at most about 20 ms is lost, more only if the writer
   had fallen behind (then up to what the 4096-event ring held). If the
   system crashes or loses power, up to the last fsync, at most 5 s. A torn
   last line is ignored; from a line that does not read on, nothing is used.
4. **Recovered as stop would have committed it.** One function places a MIDI
   take from what was captured (events, runs, how far capture got, the loop),
   used by stop and by recovery, so a recovered take is the take stop would
   have committed up to the crash: loop passes become takes of one item,
   notes still held are closed where capture ended (Note Out input).
5. **Listed and recovered with the recordings.** `project.recoverable` lists
   a journal with at least one event in `recordings` (`midi_events`: how
   many; `channels` 0). `project.recover` puts it back on the track it was
   recorded on (or `track`) as one more undo step, with the WAVs. For an
   unsaved session (ADR-0078): on its track when an autosave reads, on the
   "Recovered" channel when none does; a journal counts as something that
   can be put back. Note Out addresses of routes the project no longer has
   are dropped; the events stay. Journals are opened only through the
   recovery file check (inside the session, inside their own folder).
6. **Deleted once a saved file holds the take.** After stop commits a take
   its journal stays, not offered (the project has the take), until a save
   or an autosave is written; then the engine deletes it. Opening another
   project without saving leaves it, so it is offered the next time, as an
   unsaved audio take's WAV is.

## Consequences

- MIDI takes have the crash recovery audio takes have, through the same
  commands and popup; nothing changes for the audio thread but one store.
- A journal costs a few dozen bytes per event and per second of progress.
- **Different from audio:** a take committed and then undone before the save
  loses its journal at the save; an undone audio take's WAV stays and is
  offered as a leftover. The journal is only for crashes, not an undo store.
- **Known gap:** a crash between writing an autosave and deleting the
  journals it holds leaves a journal whose take the autosave already has;
  recovering both gives the take twice (undo removes the second).
- Recording a loop whose range changes during the take was already not
  handled (STATUS); the journal keeps the loop in effect at the start.

## Alternatives considered

- **A binary journal:** smaller, but the project's formats are JSON and a
  text journal can be read and repaired by hand.
- **Rewriting a whole file every few seconds (as the WAV header):** a
  rewrite is lost with the crash that interrupts it; appending keeps
  everything written before.
- **Writing from the audio thread:** never (RT rule).
- **Keeping journals until the user deletes them:** they would pile up and
  offer takes the project already has.
- **Marking committed takes in the project file:** a schema change for what
  only matters until the next save.
