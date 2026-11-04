# ADR-0019: Typed strips with Console 9 summing

**Status:** Accepted, 2026-09-30. Build prompt §3 "Built-in summing: Console 9
mixer strips" (added 2026-09-30) and Phase 4 "Typed strips with Console 9
built in". Supersedes the universal-track parts of `session-model.md` §1.4
and §1.8 and the TrackNode stage order of `engine.md` §1.1. Research:
`docs/research/console9.md`. Builds on ADR-0018 (the engine is stereo end to
end; routes may end at a plug-in key).

**Amendment, 2026-11-03:** [ADR-0057](0057-master-recording.md) lets masters
be armed (they record their output) and hold audio items, which play after the
plain gain/pan stage; it amends §1, §2 and §10.

## Context

Every mixer strip must have Airwindows' Console 9 built in: **Console 9
Channel** (encode, with the strip's fader and pan) and **Console 9 Buss**
(decode, neutral). Today a track is REAPER's universal track (source, bus,
folder and return at once) with a trim/volume/pan/width/mute/phase stage and
sends from three taps. Console 9 only works if every signal that reaches a
summing point has been encoded exactly once and nothing touches it until it
is decoded, so the universal track has to become typed.

## Decision

1. **Strip types.** Every track object (`tr_*`) has a `type`: `channel`,
   `buss` or `master`. Commands keep the `track.*` names (the arrange view
   still calls them tracks); "strip" is the mixer view of a track.
   - **channel** — the only type with items, takes, record arm, record
     inputs and input monitoring; never has children.
   - **buss** — sums its inputs; no items; may have children (folder-style).
   - **master** — sums its inputs; terminal (feeds hardware outputs, renders
     and the monitor chain); no items; may have children. A session may have
     several masters. `tr_0` is the main master and cannot be removed or
     retyped.
   Masters are the roots of the track tree (`parent: null`), ordered by
   `project.masters` (tr_0 first). Every channel and buss has a parent, which
   is a buss or a master; with `parent_send.enabled` its output sums into that
   parent (REAPER's folder-as-bus, kept). Type changes (`track.set {type}`)
   are validated like any edit (a channel with items cannot become a buss).
2. **Stages are implicit, never objects.** A strip's `chain` holds only user
   inserts. The compiled order, first to last:
   - channel: inputs (record/monitor) → `pre_fx` tap → items → inserts →
     `pre_fader` tap → **C9 Channel** (fader, pan, mute) → `post_fader` tap.
   - buss: input sum → **C9 Buss** → `pre_fx` tap → inserts → `pre_fader` tap
     → **C9 Channel** → `post_fader` tap.
   - master: input sum → **C9 Buss** → `pre_fx` tap → inserts → `pre_fader`
     tap → **plain gain and pan** → `post_fader` tap → hardware outputs,
     renders, monitor chain.
   The user cannot place anything before a strip's first stage or after its
   last, because no chain position exists there.
3. **Nothing between an encode and the next decode.** An edge into a buss or
   master input sum carries an encoded signal and may only be summed, channel-
   mapped and delayed for PDC. Its source is either a strip's `post_fader`
   tap (the parent send; a route with volume 0 dB, pan `follow`, no polarity
   or mono option) or a **send encoder**: the send's own C9 Channel, fed from
   its tap, with the route's `phase_invert`/`mono` before it and the route's
   level and pan as its fader and pan. A `post_fader` send encoder is fed
   from the strip's `pre_fader` signal with fader = strip volume + send
   volume (dB, clamped to +12.04) and follows the strip's mute; `pre_fader`
   and `pre_fx` send encoders use the send's level only. `route.pan` gains
   `follow` (the default: the strip's pan).
4. **Routing rules** (validated at edit time, structured errors with hints):
   audio into a channel only from `hw_input` (record/monitor inputs); audio
   into a buss or master from strips (encoded as above), never from
   `hw_input`; `hw_output` only from masters; routes into a plug-in **key**
   (ADR-0018) from any tap of any strip and never encoded (a key is a
   detector, not a summing input; a `post_fader` key tap hears the encoded
   signal). MIDI routes are unaffected. Cycles are still rejected.
   Masters cannot send audio to busses/masters; key and hardware-output routes stay allowed.
5. **Fader, pan, mute.** On channels and busses `volume` (dB, −inf..+12.04)
   and `pan` (%, −100..100) *are* the C9 Channel's Fader and Pan (mapping in
   the research note; the DSP uses the linear gain, never the native 0..1
   value). On masters they drive the plain stage: gain = 10^(dB/20) exactly;
   pan = balance, the opposite side attenuated by cos(|p|·π/2), 0 dB at the
   centre (a centred master at 0 dB is bit-transparent). **Mute** (and
   solo's implicit mute) ramps the C9 Channel's fader to −inf over 10 ms —
   before the encode, never as a gain after it; on masters the plain stage.
   The C9 Buss is fixed at neutral (A = B = 0.5) and has no fields.
   `width`, `trim` and `phase_invert` leave the strip (a utility insert does
   them); envelopes target `{track, param: volume|pan|mute}` only.
   *(Amended 2026-10-06, ADR-0022: `phase_invert` and `mono` are strip
   fields again, after the inserts and before the fader.)*
6. **Console 9 off** (level-matched; maintainer decision, 2026-09-30).
   `project.console9` (bool, default `true`). Off, every Console 9 stage keeps
   its gains and pan law but runs linear: no encode/decode curves, no
   denormal noise, no dither — the channel stage is `x · pan · f(2B) ·
   0.763932…`, the buss stage `x · 0.6180339887498949 · pan · 2` (the decode
   curve's small-signal slope), so on and off are level-matched for small
   signals and an on/off comparison hears only the summing character.
   Strip types, stage order and the routing rules are unchanged (the switch
   changes DSP, not structure): switching is a plan rebuild with the usual
   10 ms crossfade, and off costs a few multiplies per sample. There is no
   separate "plain mixer" mode; `project.pan_law` is gone (channels and
   busses use Console 9's law in both modes, masters the balance of §5).
7. **Channel counts.** The engine is stereo end to end. A mono channel
   (`channels: 1`) feeds its signal to both sides of its C9 Channel, whose pan
   places it; busses and masters are stereo. When N-channel buffers arrive,
   strips wider than two channels use the reference's mono kernels per
   channel (fader only; pan disabled for such strips).
8. **Determinism.** Each Console 9 stage owns two xorshift32 dither states
   seeded by SplitMix64 from (`project.seed`, strip id, stage, send id) —
   nonzero and ≥ 16386 as in the reference — and restarts from its seed at
   every locate (transport start, seek, render start), so real-time and
   offline renders from the same position are bit-identical. Parameter
   changes ramp per sample on the engine's mixer ramps instead of the
   reference's per-block interpolation (block-size independence; the one
   documented DSP deviation). Silence through Console 9 is seeded noise near
   −150 dBFS, not zero: silence detection uses thresholds.
9. **The port.** `libs/console9`: allocation-free, denormal-safe, float and
   double I/O, no sample-rate dependence (the reference has none), reference
   constants verbatim, optimizations only where bit-identical (constant
   pan/fader terms hoisted, `ldexp` for `pow(2, n)`). The unmodified reference
   sources live in `third_party/airwindows-console9/` and build only as a
   test target, against our own minimal stand-in for the VST2 base class (no
   Steinberg SDK); the null test runs static parameter sweeps, levels past
   the clip point, silence and tiny values, block sizes 1..4096 and rates
   44.1/48/96 kHz. Benchmarked per strip.
10. **Renders.** `render.mix {master}` (default tr_0) renders a master's
    output; the render matrix lists masters by id. A stem of a channel or
    buss is its encoded output decoded by a neutral C9 Buss (what that strip
    alone contributes to a buss); stems therefore do not sum exactly to the
    master (Console 9 is not linear summing), which the skill states.
11. **Project file 2.0.** `supple.project/2.0` adds `type`, `project.console9`
    and `project.masters`, and drops the strip `width`/`trim`/`phase_invert`.
    1.x files migrate: tr_0 → master; tracks with children or receiving
    audio routes → busses (items or record settings on such a track move to
    a new first child channel "<name> items"); all others → channels;
    `console9: true` with a warning that levels now follow Console 9's
    staging (there is no pre-Console-9 mixer to migrate to); `pan_law` and
    non-default width/trim/phase and envelopes on them are dropped with
    warnings; routes that break the rules are dropped with warnings.
12. **Credit.** Airwindows and Chris Johnson are credited in THIRD_PARTY.md,
    the skill's gain-staging page and (Phase 5) the About dialog.

## Consequences

- Invariants are structural: a plan-level checker (every buss/master input
  edge starts at a C9 Channel output and carries only sum/map/delay; stage
  order per strip type) runs in debug builds and in a fuzz test over random
  sessions (channels, busses, masters, sends of every tap, keys, PDC).
- Engine tests that check exact levels account for Console 9's staging (a
  known constant with `console9: false`, where the stages are linear); new
  tests cover the encode/decode path.
- Busses cost two Console 9 stages, channels one, send encoders one each;
  the shared-tap rule keeps unity post-fader sends free.
- REAPER-style workflows that relied on universal tracks (items on a folder
  track, a track that both plays items and receives sends) become a buss
  with a child channel; `track.add` defaults to `channel`.
