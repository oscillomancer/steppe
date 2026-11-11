# ADR-0067: MIDI sends between channels and rack note modules

**Status:** Accepted, 2026-11-10. Amends ADR-0062; builds on ADR-0064,
ADR-0065 and ADR-0058.

## Context

The maintainer needs to send a channel's MIDI to other instruments and rack
Notes modules. Note Out already emits MIDI, but the audio routing matrix
hides module sources, leaving no GUI way to connect it. Its parameter named
Channel was also mistaken for a destination track selector.

Plug-in MIDI output is being implemented separately in clap-host. The engine
must expose a post-FX MIDI stream without coupling this feature to that work.

## Decision

A non-null route `midi` is a MIDI-only send. A channel or Note Out can send to
a channel or Notes module. Audio sends remain separate, including between
the same endpoint pair. Busses, masters, keys and In modules are not MIDI
endpoints. MIDI sends accept tap, mute and channel filter/remap; MIDI learn
accepts mute only. Audio level, pan, polarity, mono and channel maps are refused.

Each channel collects timeline and monitored device MIDI plus incoming MIDI
routes independently of its audio instrument. This pre-FX stream feeds its
instrument and its pre-FX sends. A separately named, prepared post-FX stream
currently copies pre-FX; plug-in MIDI output will replace/merge it at that
seam. Pre-fader means post-FX. Post-fader additionally follows source mute;
its own mute releases delivered notes and unmute restarts keys still held.
There is no MIDI velocity scaling by the audio fader.

Notes modules hear their source channel's pre-FX MIDI plus explicit incoming
routes and their own device input. `source: "none"` disables the source;
null restores Home. The model persists this as `source_off: true`, alongside
the existing optional source track; the command clears that track when None
is selected. Project schema 2.10 defaults the new field to false for old files.

A channel's incoming sends also feed each Notes reader of that channel.
Those dependencies bypass the channel's audio node: a rack may produce the
channel's audio while reading its MIDI. Explicit and implicit dependencies
participate in mixed audio/MIDI cycle validation. MIDI edges use PDC queues,
fixed prepared event/voice storage and the existing MIDI budget error. A
source feeding a live destination runs live, keeping MIDI off the audio-only
frontier rings. Musical source identities, route paths and retained per-route note addresses
keep equal pitches independent and preserve held state across unrelated plan
edits.

Armed MIDI overdub/replace channels record delivered sends alongside optional
device input. Internal events use the destination input clock, without device
round-trip correction. Frozen channels keep playing their timeline MIDI;
both MIDI taps carry that stream while frozen, since the frozen audio print
has no MIDI output. Outgoing send edits do not affect the frozen audio hash;
timeline changes remain frozen dependencies. Incoming external MIDI still
prevents freezing.

The routing window has Audio and MIDI modes, saved with its panel layout.
MIDI mode lists channels and Note Outs as sources, channels and Notes as
destinations, and provides tap, mute and from/to-channel controls. MIDI sends
use the `midi-send` theme colour. Note Out's displayed knob becomes MIDI
channel, retaining the saved `channel` key.

## Consequences

MIDI works through relay channels without instruments, and Note Out becomes
connectable in the GUI. The engine keeps one explicit seam for future
plug-in note output. Source-off requires schema 2.10; older builds cannot
represent its semantics or the expanded endpoint combinations. Native CI
and review remain required before merge.
