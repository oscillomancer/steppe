# ADR-0065: Independent audio and MIDI inputs

**Status:** Accepted, 2026-11-07. Amends ADR-0016 and ADR-0062.
Contracts: `docs/specs/session-model.md`, `docs/specs/engine.md` and
`docs/specs/rack.md`.

## Context

A channel's single record-input route made audio and MIDI exclusive.
Choosing a keyboard on a rack's home channel replaced its Out route and
silenced the patch. Notes promises to play the home channel's monitored
MIDI, which the single route could not express alongside the rack's audio.

## Decision

Add optional `RecordSettings::midiInput` (`record.midi_input`) in project
schema 2.8. Audio stays in `record.input`. MIDI lookup prefers the new slot
and falls back to a legacy MIDI-only `record.input`; old projects need no
migration. MIDI-only channels can retain that legacy alias for existing
views. Hardware and rack audio selection preserve MIDI, and MIDI selection
preserves audio, on every channel.

A channel still records one stream at a time. If a device MIDI input is
selected, recording takes MIDI; `track.set {record_midi: null}` clears that
input to print the audio input instead. Masters keep output recording.

A rack's Notes module follows its source channel's armed monitoring and
MIDI channel map: On while stopped or playing, Auto while stopped or
recording. Its explicit device also listens without arming. Overlapping
selections deliver each input event once. Notes input changes replace the
instance through the existing plan-retirement mechanism, releasing held
gates without mutating audio-thread state from the model thread.

## Consequences

The GUI's existing MIDI-selection command no longer disconnects rack audio.
Views can show both input selections by reading the separate references.
The optional field and legacy fallback preserve old files. Copying,
validation, freezing and recording account for both references; the audio
path retains preallocated MIDI buffers and existing executor boundaries.
