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
