# ADR-0069: Hardware MIDI output device layer

**Status:** Draft, 2026-11-10. Extends ADR-0016 and ADR-0042; retains ADR-0050's
policy of leaving system-wide settings alone. Contract: engine.md, native MIDI
outputs. Native macOS/Windows compilation and hardware validation are pending.

## Decision

BackendConfig.midiOutputs opts into opening up to 16 hardware destinations;
engine.start enables it. Backend::midiOutputPorts() supplies a compact list,
fixed while running. Backend::writeMidi(MidiOutputEvent) is callback-only and
bounded: port index, frame within that engine callback, size and ten inline
bytes. Complete MIDI 1.0 channel, system-common and realtime messages, and
complete SysEx up to ten bytes, are supported. This includes clock, start,
continue, stop, SPP, quarter-frame MTC and full-frame MTC. No running status.
Larger SysEx is outside this interface. Routing, clock generation and GUI
controls are separate work; this change opens devices but emits no music itself.

There is one 1024-event acceptance budget per whole device period, across all
engine chunks. Invalid ports, frames and messages and excess events are
rejected and counted. Fixed storage is stably sorted by period frame at flush;
equal-frame events retain call order. Native output has a preallocated 2048-event
SPSC queue per destination. Audio only validates, copies, sorts and publishes;
no OS output API, allocation, mutex, wake syscall or wait runs there. Failed
queue writes, device sends, a backwards per-port timestamp, and pending queued
events discarded by stop count in Backend::midiOutDropped(). Counts reset on
start. OS-accepted events cancelled at shutdown cannot be counted precisely.

### Latency and clock

Backend::midiOutputLatency() returns a signed frame value per port:
**audio output latency minus MIDI port latency**. For equal cycle frame f,
positive L means MIDI leaves L frames before audio reaches the speakers. To
align them, future routing delays MIDI by L plus a signed user offset. Negative
L requires advancing it; crossing a period boundary requires routing lookahead
or delay storage. The device layer applies no hidden alignment, avoiding double
compensation. It cannot manufacture already elapsed time. This is departure of
the first MIDI byte, not a claim about a synth's sound-generation latency.

JACK uses playback latency range maxima of the first audio output and each
MIDI output; absent optional latency symbols mean zero. CoreMIDI and WinMM
expose no measured wire latency in this integration, so MIDI latency is zero
and the reported difference is audio output latency. Those are estimates;
user calibration belongs in routing. CoreMIDI's recommended advance-schedule
time is a lead-time requirement, not measured MIDI port latency.

Native callback frame zero is anchored in the sink's monotonic clock (Mach
host ticks or QPC) at entry, plus PortAudio's
`outputBufferDacTime - currentTime - audioOutputLatency`. Frame f adds f/rate.
The difference bridges PortAudio's possibly different clock epoch. Device-free
callbacks without time info use entry time. Engine chunks add their period
offset exactly once. Native queues preserve per-port nondecreasing due times;
a backwards clock discontinuity is dropped, rather than allowing an earlier
message to sit behind a future head.

### Platforms and lifecycle

Linux optionally binds jack_midi_clear_buffer/jack_midi_event_write, registers
midi_out_N and connects to physical MIDI input/playback ports. Failed port
registration or connection produces a structured warning and omits that
output. The process callback clears every opened output each period, then
writes sorted period events and counts JACK buffer rejection. Optional JACK
port registration notifications are conservative topology advisories (audio
port registration can trigger the same flag), never automatic restarts.

On macOS, one output port shares the input's client. Client reference counting
and port creation/disposal occur on the owner thread. MIDI 1.0 channel voice
uses UMP type 2, system common/realtime type 1, and SysEx7 type 3 (ten-byte MTC
uses start/end UMPs). A sender thread calls MIDISendEventList with future Mach
host timestamps. Apple's documentation promises future scheduling but gives
no realtime safety guarantee for the send call; absence of that guarantee is
why it stays off the audio thread. The shared queue path is tested on Linux.
At stop, audio is joined, the sender is joined, scheduled endpoint output is
flushed, the output port is disposed, and the client's last user releases it.
CoreMIDI's endpoint flush may also cancel output another client scheduled to
that endpoint; this API has no per-port flush operation.

On Windows, each device uses midiOutOpen, and one MMCSS Pro Audio sender
(high thread priority fallback) sends due short messages or long SysEx buffers.
It waits with a process-owned CREATE_WAITABLE_TIMER_HIGH_RESOLUTION timer,
without timeBeginPeriod. Failure to create the timer reports a startup warning.
Eight prepared ten-byte SysEx buffers per port avoid unbounded in-flight
ownership; callback completion only returns a buffer with an atomic store.
All prepare/send/reset/close calls are outside audio. Stop joins the sender,
then midiOutReset returns in-flight buffers, unprepares them and closes ports.
Reset may send all-notes-off on the device, as WinMM specifies. Expected timing
jitter is about 1 ms, not sample accuracy; physical measurements are pending.
Windows MIDI Services remains outside this change.

The sender polls at most every 1 ms, merging the earliest queue heads. CoreMIDI
submits future events immediately for OS scheduling; WinMM waits until due.
Shutdown discards future queued output instead of waiting for its deadlines.
An event due before the sender sees it is delivered late; accurate future
scheduling requires enough lead time, especially for CoreMIDI destinations
that advertise advance-schedule time. No hardware accuracy claim follows from
unit tests.

Opening is best effort per port, with numeric OS errors, device, message and
corrective hint in engine.start/status.midi_output_warnings. Only opened ports
are listed. Output topology is advisory through midi_devices_changed, alongside
input topology; the native monitor keeps running and audio is uninterrupted.
WinMM retains the existing identical-name/replacement detection limits.

## Validation

Tests use fake JACK, DummyBackend capture with absolute frames, fake PortAudio
and injectable MidiSink clocks. They exercise message validation, stable sort,
chunk mapping, latency signs, queue and period overflow, send rejection,
shutdown of pending events, startup failure, sender lifecycle, and command
serialization. The shared callback code runs under RTSan; concurrency/lifecycle
under TSan. SUPPLE_MIDI_DEVICE_TEST=1 opens native output devices with fake
audio and sends no messages during the test. Closing a WinMM output may issue
the reset described above. Native CI and device trials are required before
merging or claiming platform/hardware behavior verified.

References: [Apple send scheduling](https://developer.apple.com/documentation/coremidi/midisendeventlist(_:_:_:)),
[host timestamps](https://developer.apple.com/documentation/coremidi/miditimestamp),
[advance scheduling](https://developer.apple.com/documentation/coremidi/kmidipropertyadvancescheduletimemusec),
[WinMM output messages](https://learn.microsoft.com/en-us/windows/win32/multimedia/sending-individual-midi-messages),
[WinMM prepared buffers](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-midioutprepareheader),
[high-resolution timers](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw),
[JACK MIDI](https://jackaudio.org/api/group__MIDIAPI.html),
[JACK latency](https://jackaudio.org/api/group__LatencyFunctions.html).
