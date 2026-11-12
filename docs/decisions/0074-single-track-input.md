# ADR-0074: A track has one input

**Status:** Accepted, 2026-11-11. Supersedes ADR-0065's independent inputs
and legacy rack reconnection. Amends ADR-0062.

A channel chooses one input: hardware audio, hardware MIDI or a rack Out.
`track.set` of one kind replaces the others; providing multiple non-null
choices in one call fails with `track.record_input`. Null clears only its
named kind. The GUI presents one No input action that clears all kinds.
Adding a rack selects its new Out, replacing a previous input.

MIDI sends into Notes now provide the way to play a rack from a keyboard:
a separate channel selects and monitors the keyboard, and sends MIDI to
Notes. The rack channel retains its Out and can record audio, while the
keyboard channel can record notes. Notes' own explicit device selection
remains available.

Project schema 2.12 validates one distinct input route per channel. The
MIDI-only `record.input` alias of `record.midi_input` names the same input
and remains accepted for compatibility. Older files with conflicting input
references keep rack first, otherwise MIDI when the main FX chain is nonempty,
otherwise audio. The maintainer chose a nonempty main chain as the migration
signal: project files do not store plug-in note-input capabilities, and
migration must work without scanning or opening plug-ins. Each conflict
emits `project.input_conflict`, naming the track and dropped input; removed
routes take their envelopes and automation items with them. Warnings name the
dropped kind/device and suggest another channel with a MIDI send to Notes.
The <=2.8 lost-Out detector reconnects an unambiguous rack Out and drops the
MIDI route, giving the same rack-only result as a 2.10 resave of the earlier
repair. Ambiguous or invalid reconnections retain MIDI; invalid ones warn
with recovery advice. No loader creates a rack-plus-keyboard pair.

Recording, monitoring, routing cells and agent edits use this same selection.
Internal MIDI sends remain independent routes rather than extra hardware
inputs. Native CI and review are required before merge.
