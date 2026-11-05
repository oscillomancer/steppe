# ADR-0045: Master playrate in device-time graphs

**Status:** Accepted, 2026-10-16. Implementation is staged: model and commands
precede DSP integration. Until that integration, playback and rendering remain
at 1x even when the project stores another rate. No GUI is introduced by the
model/command stages.

**Automation amendment, 2026-11-05:** [ADR-0061](0061-automation-write-holds.md)
holds strip, send and VCA levels and pans while they are written; the
playrate keeps the coarse writing of decision 7.

Amends the transport, automation and recording contracts in ADR-0012,
ADR-0014 and ADR-0015. Preserves ADR-0019's Console 9 staging and ADR-0024's
separate live executor. Contracts: `specs/engine.md`, `specs/session-model.md`.

## Context

Master playrate must change project speed, optionally preserve audio-item
pitch, support automation, and leave live audio/MIDI monitoring in device
time. Both executors and offline rendering must produce identical samples.

A converter after the master would also convert monitored input. Moving it
to renderer-to-live frontier edges would process Console 9 encoded audio
before decode, contrary to ADR-0019. It would also change the order relative
to nonlinear master/buss effects when monitoring changes. Even a silent
monitored input would change the result. Live channels can themselves contain
timeline audio and MIDI, so an empty frontier does not remove this problem.

## Decision

1. **One clock, device-time graph.** Keep backend/plugin sample rates and
   graph buffers in device samples. Add a fractional timeline clock, distinct
   from the monotonic device stream and source lookahead. Source playback,
   timeline MIDI, automation and transport share its mapping. At exactly 1x
   without an active nonempty rate envelope, retain the existing exact path.
2. **Convert sources before mixing.** Original streaming tape resampling or
   the vendored MIT Signalsmith Stretch converts each audio take/source before
   take FX and before it joins live input. Never insert conversion between a
   Console 9 encode and decode. Instruments retain their note pitch; effects
   run in device time. Timeline MIDI uses mapped device offsets, live MIDI
   retains its original offsets, and plugins receive effective BPM equal to
   project BPM multiplied by effective playrate.
3. **Both executors.** Source conversion runs with its node on the renderer
   or callback/live helpers. Frontier rings retain device-domain audio and
   their existing encode/sum/decode order. Offline uses the same clock and
   conversion, including the same canonical call schedule, floating-point
   environment and seed/reset policy. Analysis taps and item-FX renders opt
   out of master playrate.
4. **Bounded streaming state.** Allocate coefficients, histories, windows,
   scratch and output buffers before publication. Use canonical conversion
   quanta independent of backend/render block sizes (initially 64 device
   frames). Maintain fractional consumption across calls. Signalsmith's
   `reset()` does not reseed its random phase generator; deterministic seek
   needs an audited reset wrapper or a documented minimal MIT vendor patch.
   A loop wrap retains DSP history, unlike a new seek epoch.
5. **Mode changes.** Prime the newly selected converter from available file
   source content, then crossfade for 10 ms at a canonical boundary without
   adding graph latency. Do not keep both converters continuously warm.
   Prepare source-only converter state off the audio callback, including
   callback-owned sources; live monitoring continues while preparation runs.
   Delay publication until every newly selected source is ready. The same
   transition applies when leaving unity bypass. Preserve compensated source
   alignment: schedule the shared content-time transition after the maximum
   compensated source lookahead, then coalesce newer requests until all sources
   finish the transition. Long-latency take chains can therefore exceed the
   usual approximately 200 ms control response. Requested settings update
   immediately; effective rate follows consumed output. Prepare fresh seek
   state off the callback and delay the audible seek until it is ready; live
   monitoring continues throughout. Reserve clock history for ordinary multi-second
   PDC changes. If a graph edit exhausts clock/converter history or repeatedly
   misses its prepared boundary, recover automatically through a prepared seek at
   the consumed position, with the same state reset/preroll as a locate. No user
   pause/reload is required. No callback calls a synchronous bridge or the
   background pool.
6. **Saved settings.** `project.playrate` holds `{rate, preserve_pitch}`.
   Rate is finite and bounded to 0.25..4.0, default 1.0; preserve-pitch defaults
   false. This matches a fresh private REAPER 7.82/Linux Dummy Audio experiment:
   action 40671 reports off, and its saved project has `PLAYRATE 1 0 0.25 4`.
   Project format 2.1 adds the settings; older projects default to 1x/off.
7. **Envelope.** The unique master rate target is `tr_0`, parameter
   `playrate`; other tracks, routes and takes cannot target this parameter.
   Its values are ratios evaluated in timeline time. Existing envelope
   timebases, shapes and automation items apply. Base points and scaled
   automation-item output must stay in range on edits and project loading.
   Write/touch/latch use existing coarse strip-writing semantics: moves write
   points and latch/write hold through stop; no new touch override/return
   machinery is promised.
8. **Commands.** `transport.playrate.get` reports requested/effective rate,
   preserve mode and active nonempty envelope; `transport.playrate.set` is a
   project edit with optional rate/preserve fields. The effective rate is
   what audio consumption has reached, not lookahead or the latest request.
   In the staged control-only implementation it is explicitly 1.0.
   `transport.playrate.apply-to-tempo` scales every tempo-point BPM by the
   saved scalar rate, retaining beat positions, ramps and meter changes, then
   resets rate to 1 in one version. Require stopped transport, no recording
   and no active nonempty rate envelope. Reject resulting BPM outside 1..1000
   before mutation. Existing item timebases still apply; this is not an
   audio-preserving bake for time-locked items.
9. **Recording guard.** Refuse audio and MIDI recording with `record.playrate`
   when the saved rate differs from 1 or an active effective rate envelope
   leaves 1 anywhere. Reject edits that would introduce that condition during
   recording, before committing or opening files. Apply the guard at app and
   engine boundaries; history/branch changes must not bypass it. Variable-rate
   recording is deferred: capture placement, MIDI, loop/punch segments and
   recovery sidecars currently assume one timeline sample per file frame.
10. **Audible state.** Keep the existing render horizon (normally 200 ms).
    A knob edit and its clock metadata become audible at the same boundary.
    Manual changes ramp over 10 ms in device time; authored square envelope
    steps keep their shape. Stream/device coordinates remain monotonic across
    loops. Telemetry, meters and plugin-output delivery map consumed output
    through that clock; hardware latency remains in device frames.
11. **Click and render bounds.** Generate metronome clicks in device time at
    mapped beat crossings, retaining click pitch. Render range bounds remain
    timeline seconds; emitted duration follows the inverse clock. Tails are
    device seconds. Check expanded output budgets at the minimum rate.
12. **GUI handoff.** A later GUI change orders meter, tempo, knob and Rate
    readout. Double-click resets 1x. Context options are reset, semitone and
    ten-cent steps, tempo bake, preserve toggle and four display ranges.
    Range selection belongs to a transport section of the per-project UI
    sidecar, not project history, and never clamps the saved/automated rate.

## Consequences

- This is an engine clock/source feature, not a master effect. MIDI, source
  playback, automation, PDC, loops, event timestamps and telemetry must share
  the mapping. Stock/test plugins retain exact RT/offline equivalence.
- New clock/converter tests are shared on Linux, macOS and Windows; Linux-only
  realtime tests alone do not establish portability. New audio-path tests run
  under RTSan. Native CI and independent review remain release requirements.
- The model/command stages are useful for GUI integration but do not yet
  change audio. STATUS and command descriptions must state that limitation.
- Real-time source priming, extreme-rate memory/CPU and spectral quality need
  explicit tests. Arbitrary wall-clock knob edits are equivalent offline only
  when compared against the same captured envelope/control trajectory.

## Alternatives considered

- Whole-master conversion: simpler for playback-only sessions, but converts
  monitoring and changes instrument/FX pitch/timing. Restricting live graphs
  would fail the requested feature.
- Frontier conversion: violates Console 9 and changes sound with graph
  classification. Decode/convert/re-encode introduces extra nonlinear stages.
- Duplicate playback/monitor graphs: loses shared nonlinear FX, sidechain and
  instrument-state behavior. A pre-encode split eventually needs the same
  source/event clock work as the chosen design.

**2026-10-17 amendment:** [ADR-0046](0046-live-scalar-playrate.md) replaces
prepared-plan/coalescing delivery for compatible scalar-only live edits with
scheduled clock controls and specifies renderer-frontier handoff scheduling.
