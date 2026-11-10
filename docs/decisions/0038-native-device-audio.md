# ADR-0038: Native device audio through PortAudio

**Status:** Accepted, 2026-10-14. Amends ADR-0004 and ADR-0035's device/helper
scheduling boundary. Linux JACK behavior is unchanged.

**Monitor clarification, 2026-10-14:** Native driver rate queries, including the
initial baseline, run exclusively on the monitor. It publishes rate/loss flags
through atomics; owner-side status reads them and cheap PortAudio stream
info/activity. Owner status never queries the ASIO/CoreAudio driver rate.

**2026-10-15 amendment:** [ADR-0042](0042-native-midi-input.md) adds CoreMIDI /
WinMM inputs to the PortAudio backend and supersedes the MIDI deferral below.

**2026-10-25 amendment:** [ADR-0050](0050-audio-device-preferences.md) makes the
GUI start the native device by default (headless engine.start keeps dummy),
asks CoreAudio for an explicit rate and lets WASAPI shared mode convert rates.

**2026-11-10 draft extension:** [ADR-0069](0069-native-midi-output.md)
adds the hardware MIDI output device contract; routing and clock generation
remain separate work. Native output validation is pending.

## Decision

- Vendor the latest eligible stable PortAudio release, v19.7.0 (2021-04-06,
  commit 147dd722548358763a8b649b3e4b41dfffbcfbb6), under its MIT license.
  Build only CoreAudio on macOS and WASAPI + ASIO on Windows, statically into
  the DAW. Never build the device library on Linux or in isolated plugin
  projects. The Windows target includes the sanctioned GPL-3.0-only ASIO SDK
  (ADR-0033); its complete build is DAW-only. No DirectSound, MME or WDM-KS
  host APIs are enabled; common Windows format/utility code is still needed.
- One backend name, `portaudio`, with exact `host_api`, `input_device` and
  `output_device` names. Empty names select the host API's defaults. Native
  engine.start defaults to dummy; a portaudio request explicitly opens a stream.
  Linux retains its existing JACK default. Inputs default to zero for native
  playback, outputs to two, rate to the project rate. Native MIDI is deferred.
  `engine.devices` enumerates names, channel capacities, default rate and default
  input/output indicators, without opening a stream. Its dummy example never
  initializes hardware. Duplicate device names within a host API are rejected.
- Request non-interleaved float32, disable PortAudio dither/clipping, and split
  every callback into engine-block-sized chunks with a final short chunk.
  Pointer arrays are bounded stack storage. No allocation, lock, wait, file
  access, OS scheduling change or synchronous log occurs in our callback.
  Device flags count affected callbacks, not individual flag bits. Period,
  latency and xrun counters remain distinct from engine process errors/misses.
- A control-side monitor observes native stream state every 250 ms while a
  requested device stream exists. CoreAudio nominal rate and ASIO current rate
  supplement stream information; WASAPI invalidation is observed through stream
  termination. Changes publish atomics and wake the owner's existing relay for
  restart, retaining the device names, rate and channel recipe. There is no
  main-thread polling timer and the monitor is joined before closing the stream.
- Normal stop requests a final zero-frame engine callback on the next device
  cycle, returns paComplete, and closes/joins the stream off the audio thread.
  A stopped/lost/stalled device uses the JACK-style lifecycle fallback after
  callback shutdown: close/join first, then final cleanup on the owner. The
  fallback is reported through the existing device-loss/restart state rather
  than pretending a dead device delivered a callback.
- Device callback scheduling belongs to the host API. Owned live/background
  threads apply their policy themselves, after opening the device and before
  receiving jobs. A startup barrier checks every helper and rolls a refusing
  helper class back before publication. Windows uses MMCSS `Pro Audio` (critical
  live helpers, high renderer/background helpers), reverting registration at
  thread exit. macOS uses Mach time constraints derived from the opened stream's
  rate and engine block (live computation budget one third of its period,
  background one sixth, constraint one half, preemptible). This resolves the
  cross-thread QoS fallback in ADR-0035 without requiring workgroup membership.
  Failed live setup retains serial execution with an actionable warning;
  background failures report normal-policy fallback. Scheduling status uses
  `high` for these successful native policies; it is not Linux FIFO priority.
  Linux helper APIs, priorities, affinity limits and realtime safeguards remain.

## Validation and limits

An injected driver exercises the same selection, open/close, planar chunking,
xrun/period/latency, rate/loss, resource-failure and shutdown code without loading
or initializing PortAudio. Its engine output must be bit-identical to offline
in both executors, with Console 9, plugin latency and several worker counts.
Forced helper-policy refusal covers startup rollback and owning-thread setup.
Run callback tests under RTSan and concurrent lifecycle tests under TSan.
Native builds verify the actual library/SDK integration. A real-device test on
each native platform runs only when SUPPLE_AUDIO_DEVICE_TEST is exactly `1`.
Hardware latency, driver stability and helper deadline benefit need those
user-run device trials; fake tests do not establish them. Device names can change across reconnects; users
can select defaults or update the retained recipe.

## Alternatives

Separate CoreAudio/WASAPI/ASIO backends would duplicate device and stream
lifecycle code. PortAudio is permissively licensed and supplies all three
host APIs. Its API table keeps the callback contract testable on Linux while
leaving JACK and plugin linkage unchanged. Self-applied native scheduling
supports MMCSS, which cannot register another thread. macOS time constraints
are the initial mechanism; the workgroup join was added on 2026-10-15 (below).

## Amendment (2026-10-15): saves while another program holds the file

The Windows limit that readers denying delete sharing block atomic replacement
no longer holds as a hard failure. Antivirus, indexers, sync clients and
viewers hold a just-written file open briefly, so every replace step
(`writeFileAtomic`, the audio-file RF64 publish and the engine's link-style
publish) retries sharing and lock violations, and access denied on an existing
file, with a short backoff for about one second through one core-base helper
(`retryWhileFileBusy`). A hold that outlasts it returns the usual `file.write`
(`audio.write`) error with the path, saying another program has the file open
and to close it and save again; the previous file is unchanged. Linux and macOS
are unaffected.

## Amendment (2026-10-15): macOS live helpers join the device workgroup

On Apple Silicon a thread outside the device's audio workgroup can be placed on
an efficiency core or run late, whatever its time-constraint policy. After
PortAudio opens a stream, the backend reads `kAudioDevicePropertyIOThreadOSWorkgroup`
from the output device (`PaMacCore_GetStreamOutputDevice`). Each live helper
joins it with `os_workgroup_join` from its own thread inside the existing
startup barrier (`prepareCurrentThread(live = true)`), after the time-constraint
policy (a join without it fails with EINVAL), and leaves in
`releaseCurrentThread`. The token is thread-local. The renderer and background
pool do not join: they work ahead of the device cycle and are not bound by its
deadline, so claiming its interval would mislead the scheduler. Nothing on the
per-block path calls the workgroup. The reference the property returns is not
released (its ownership is undocumented; one leaked reference per stream is
harmless, a double release is not).

A refused lookup or join never fails the start and never rolls helpers back:
the helper keeps the time-constraint policy. `engine.status.workgroup` reports
`state` (`none`, `ready`, `joined`, `partial`, `failed`), the joined and
refused helper counts and the last refusal's OS error, message and hint, and a
partial or failed state adds a start warning. Other platforms and backends
report `none`.

**Bridge (ADR-0037), not done.** Bridged plug-ins process on the bridge
process's audio thread for the same deadline, and `os_workgroup_copy_port` /
`os_workgroup_create_with_port` can carry a workgroup between processes. But a
Mach port right travels only in a Mach message or through the bootstrap server;
the bridge's control channel is a Unix socket and its audio transport shared
memory, so the join needs a second IPC mechanism (a per-bridge bootstrap
service or a Mach-message handshake at launch). The bridge's audio thread also
runs under the renderer's policy and takes no time-constraint policy, which a
join requires. That is a bridge-design change that cannot be validated without
an Apple Silicon device trial, so it stays out until the join above has been
measured on hardware. Revisit then.

Tests: a fake workgroup exercises join/leave on the helper's own thread, the
live-only rule, refusal and lookup failure, and status/warning serialisation
(all platforms); on macOS a real `os_workgroup_parallel_create` workgroup
covers the wrapper's join/leave/rejoin lifecycle; the
`SUPPLE_AUDIO_DEVICE_TEST=1` path asserts the join on a real device.
