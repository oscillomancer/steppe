# ADR-0065: A rack's channel keeps its MIDI input beside its audio

**Status:** Accepted, 2026-11-07. Amends ADR-0016 and ADR-0062.
Contracts: `docs/specs/session-model.md`, `docs/specs/engine.md` and
`docs/specs/rack.md`.

## Context

A channel's single record-input route made audio and MIDI exclusive.
Choosing a keyboard on a rack's home channel replaced its Out route and
silenced the patch. Notes promises to play the home channel's monitored
MIDI, which the single route could not express alongside the rack's audio.

Making every channel's inputs independent went too far. A keyboard channel
switched to a microphone kept its MIDI input; with MIDI preferred, the vocal
take was never written.

## Decision

Add optional `RecordSettings::midiInput` (`record.midi_input`) in project
schema 2.8. Audio stays in `record.input`. MIDI lookup prefers the new slot
and falls back to a legacy MIDI-only `record.input`; old projects need no
migration. A MIDI-only channel keeps `record.input` as an alias of its
`midi_input`, so older views and applications still see it. A MIDI
`record.input` must be that alias.

Hardware audio and MIDI stay exclusive, as before: `track.set
{record_input}` turns the MIDI route into the hardware one, and
`{record_midi}` turns the hardware route into the MIDI one. A rack's Out is
the only audio input that keeps a MIDI input beside it, in either order of
selection.

Such a channel records MIDI: a patch is generally either generative or a
keyboard instrument, and with a keyboard selected the played notes are what
is wanted. Without a MIDI input it records the rack Out's audio as a WAV
take. To record both in one pass, a second channel takes the rack's Out as
its input and is armed beside the first.

A rack's Notes module follows its source channel's armed monitoring and
MIDI channel map: On while stopped or playing, Auto while stopped or
recording. Its explicit device also listens without arming. Overlapping
selections deliver each input event once.

When a Notes module's live input changes (arming, monitor mode, device or
MIDI channel), the instance stays and its live-input generation advances.
The audio thread sees the newer generation in the feed and ends only what
live events opened: their voices, held keys, and the bend, mod wheel,
pressure and sustain they set. Timeline notes keep playing. When a rack
node enters the anticipative engine's live region mid-song, it continues
where the renderer left it (ADR-0014's class changes), so arming a channel
during playback does not cut them.

Before this change, choosing a keyboard on a rack's channel turned the Out's
route into the MIDI route. Loading a 2.8 or older file repairs that state
with a `project.rack_input_repaired` warning when it is unambiguous: a
channel hosting a rack whose `record.input` is a MIDI route, with no
`midi_input`, and whose rack has exactly one Out with no route. The MIDI
route moves to `midi_input` and a new route from the Out becomes
`record.input`. If the repaired project would be invalid, the file loads as
it was, with `project.rack_input_unrepaired`.

## Consequences

The GUI's existing MIDI-selection command no longer disconnects rack audio,
and choosing a hardware input on a keyboard channel still records audio.
Views can show both selections on a rack channel by reading both references.
Copying, validation, freezing and recording account for both references; the
audio path keeps preallocated MIDI buffers and existing executor boundaries.
