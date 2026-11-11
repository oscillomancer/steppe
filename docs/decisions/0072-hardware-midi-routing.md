# ADR-0072: Hardware MIDI routes, port preferences and causal alignment

Date: 2026-11-11
Status: Proposed; native CI and hardware timing trials pending
Amends: ADR-0067 (MIDI sends), ADR-0069 (native output shutdown)

## Decision

Extend the unreleased project schema 2.10 with MIDI routes whose destination
is `to.midi_device`. Channel and rack Note Out sources use the same taps,
channel filter/remap and mute semantics as internal MIDI sends. Device names
match case-insensitively, as inputs do. Missing destinations warn without
preventing playback. Audio and offline rendering keep their existing signal
path; offline renders never open or write devices.

Hardware sources and their MIDI dependencies run in the callback in both
executors. Prepared route descriptors reference the pre/post-FX voice streams
and rack Note Out events; audio never looks up names, changes preferences,
allocates storage or talks to a native MIDI API. A fixed, stable min-heap holds
at most 65,536 scheduled events across at most 256 hardware routes; the backend
retains its 1,024-event period budget and 16-port bound. Loss is counted.

For a source event, the delay in frames is the plan output latency minus its
source timeline shift and tapped latency, plus the backend's signed
audio-minus-MIDI latency, plus the user offset. A negative minimum delay needs
causal lead: add its magnitude to every hardware delay and to a prepared audio
delay after the callback mix. The audio delay is allocated before publishing a
plan and bounded to one second at the granted device rate. Greater advances
fail with an actionable error. A hardware delay above ten seconds also fails.
The added audio latency is reported as `midi_lead_frames` and included in the
engine output latency, heard clock and recording compensation. Aligning the
captured audio by the reported output latency leaves the samples unchanged.
Changing the lead clears the delay; playback/locate clears its old tail.

Input timing uses the existing track record MIDI routes. A positive offset
carries events across callbacks in another bounded heap. A negative offset can
advance only within the current callback and clamps at its start: a real input
cannot arrive before it is received. Disabled inputs are filtered before
monitoring, recording and MIDI learn, and their compiled port masks are cleared.

Track held notes per hardware route, including remapped channels and duplicate
keys. Route removal, mute, disabling/re-timing and engine shutdown release
delivered notes. Stop/locate releases them when the port's `reset_on_stop` is
true (default); false leaves timeline notes held until subsequent events or
engine shutdown. Releases include sustain-off; transport resets and shutdown
also send all-notes-off on used channels. These controllers affect a MIDI
channel, so routes or other clients sharing that hardware channel share its
controller state. Use separate channels when independent sustain is required.
Pending events belonging to a removed/reset generation are discarded. Bounded
period overflow may require subsequent callbacks to finish individual offs;
shutdown also uses channel reset messages within the reserved panic budget.

CoreMIDI must never call the endpoint-wide `MIDIFlushOutput`. The engine's final
callback emits releases, and the sender drains releases after already accepted
messages, preserving each port's scheduling order. Already submitted CoreMIDI
events drain normally; unused future queue messages are discarded. Dispose only
the owned port/client. WinMM retains its owned-handle reset/close behavior.
This amends ADR-0069's endpoint-flush shutdown policy.

## Preferences and UI

Use additive fields in `supple.preferences/1.6`: `midi_outputs` and
`midi_inputs`, keyed by port name. Output records have `enabled` (true),
`offset_ms` (0, finite -1000..1000) and `reset_on_stop` (true). Inputs have
`enabled` and `offset_ms`. Preserve unknown record members so Phase 3 clock,
transport, spp, mtc and clock_while_stopped fields survive these edits.
Settings belong to this machine and are not stored in projects or undo history.
Commands `midi.inputs.get/set` and `midi.outputs.get/set` expose them and engine
status includes per-port state, timing, attributed loss and warnings.

The third routing tab, Hardware MIDI, has hardware input rows followed by
channel/Note Out rows, and channel columns followed by hardware outputs. It
uses the existing input assignments; one input assignment per track is retained.
Input-to-output cells are blocked: use a channel between them. Notes module
columns remain in the MIDI tab. Hardware header context menus expose enabled,
offset and output reset settings. `RoutingMatrix::midiPortMenu` is the extension
seam for Phase 3; output descriptors already carry an optional clock flag for
the output header's clock mark.

## Validation

Device-free tests cover timing and audio equality in both executors, taps,
mute and lifecycle releases, preferences and project round trips, offset queues,
native sender shutdown and the hardware tab/header menu. Native CI, physical
latency/jitter, disconnects and cross-client shutdown on macOS/Windows must be
validated before merge; Linux fake devices do not establish hardware accuracy.
