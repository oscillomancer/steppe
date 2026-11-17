# ADR-0028: Renderer deadlines and event-driven wakes

**Status:** Accepted, 2026-10-08. Amends ADR-0012 §5 and ADR-0024 §4.
Spec: `docs/specs/engine.md` §2.1.

**Amendment, 2026-11-14:** [ADR-0081](0081-live-bridged-plugins.md) lets
the callback process bridged plug-ins on live nodes, with one bounded wait
per cycle; it states the exception to the callback's no-wait rule.

**Platform amendment, 2026-10-14:** [ADR-0035](0035-headless-platform-layer.md)
adds macOS/Windows mechanisms while retaining these Linux semantics.

## Context

The renderer polls every millisecond while stopped and every quarter render
block while its playback buffer is full. Stopped plug-ins still need regular
processing for editors and sample loading. Buffer consumption is an event.
The existing private futex wake is already allowed on callback threads.

## Decision

- Wait on a futex notification until work arrives or a deadline expires.
  A stopped renderer processes at render-block pace. Pending process/flush
  requests and queued parameter edits may bring the next block forward, but
  no sooner than 1 ms after the previous stopped block finishes. A request
  made inside process() therefore cannot cause back-to-back requested blocks.
  A notification without pending audio work does not trigger an extra block.
- While playing, wait until the callback has consumed enough audio for another
  render block. A full master/frontier ring must release a chunk first. The
  callback wakes once at that consumption point, rather than every cycle.
  Old chunks discarded after a seek can also free space. An all-live graph
  needs no buffer wakes; its edit horizon catches up when a new plan arrives.
- Keep a safety timeout for missed notifications. During playback it is half
  the effective render-ahead span, normally 100 ms, so it expires before the
  buffer drains. Stopped, held, planless and all-live renderers keep a 1 s
  bound. Stopped processing still wakes at its earlier block/request deadline.
- Plan publication, completed transport writes, play/count-in, hold/release
  and shutdown wake the renderer. The callback also wakes it when acknowledging
  an epoch or releasing instance ownership. Relocation waits for that
  acknowledgement instead of polling.
- Subscribe plug-ins through main-thread `Instance::setAudioWake`. Local CLAP
  requests and queued parameter edits notify after publication. Active VST3
  and bridged parameter edits carry an explicit pending process request.
  Bridged plug-in requests notify when existing main-thread service receives
  them; the bridge protocol stays unchanged. Detachment waits for outstanding
  notifications before an instance or engine's wake word can be freed.
- Read the sequence before checking work, and publish state before notifying.
  For buffer space, arm and recheck with sequentially consistent consumption
  operations: either the recheck sees freed space or the callback sees the arm.
  Code comments describe the ordering and notification-lifetime handshake.
- Extend the existing `rt::wake` syscall exception to renderer notifications.
  The callback never waits, writes an fd, allocates, or locks. DSP ordering,
  epochs, ownership and the render-ahead edit horizon remain unchanged.

## Consequences

The renderer waits for a useful event or a processing deadline. Repeated
plug-in requests are bounded without a polling timer. Safety timeouts provide
insurance, not normal pacing. App servicing and MIDI polling remain separate
work. Tests check computed deadlines and notification counts without using
elapsed time to decide pass or fail, alongside exact-output and real-time-safety
tests.

## Alternatives considered

Short periodic sleeps wake without work. Callback writes to a pipe/eventfd
violate the current audio-thread contract. Unbounded waits leave no insurance
for a missed notification. A 1 s playback timeout can let the buffer drain.
