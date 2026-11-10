# ADR-0066: Pan modes, selectable pan laws and unity Console 9 staging

**Status:** Accepted, 2026-11-10. Amends ADR-0019 §5, §6 and §9 and
`docs/research/console9.md`'s gain staging. Contracts:
`docs/specs/session-model.md`, `docs/specs/engine.md`, `docs/specs/gui.md`
and `libs/console9/README.md`.

## Context

Every Console 9 hop lost 6.52 dB at the centre for small signals. Of that,
6.02 dB came from two pan laws: the C9 Channel's sin/cos law (−3.01 dB at the
centre) at the source, and a second one in the C9 Buss, fixed at the centre,
which nobody could move. The remaining 0.50 dB was the net of Airwindows'
own trims, ×0.7639 before the encode and ×2 after the decode, around the
curve pair's ×0.618. A channel → buss → master path came out 13 dB down, and
every extra buss cost another 6.5 dB.

The only pan law was the Console 9 one, so a project could not choose a law,
and a stereo strip could only balance its two sides, never place, narrow or
widen its image.

## Decision

1. **Selectable pan laws, as REAPER offers them.** A law is `{db, boost}`
   with `db` one of 0 (the default), −2.5, −3, −4.5 or −6. For a mono
   source at position p ∈ [−1, 1] with θ = (p + 1)·π/4, the right side's
   gain is `f_R = S_R^k · B_R^max(0, 1 − k)` and the left side's is the
   mirror image, where:
   - `S_R = sin θ` (constant power);
   - `B_R` is 1 for p ≥ 0 and cos(p·π/2) otherwise. That is the balance
     curve masters already use, which never boosts;
   - `k = −db / 3.0103`, with the labels −3 and −6 meaning k = 1 and k = 2
     exactly.
   So 0 dB is balance (unity at the centre), −3 dB is sin/cos constant
   power, −6 dB is sin² constant amplitude, and −2.5 and −4.5 dB land
   exactly on their label at the centre. **Boost** ("gain compensation")
   divides by the centre gain 2^(−k/2): the centre is 0 dB and the sides
   are raised by |db|. At 0 dB, boost changes nothing.
2. **Where laws are set.** `project.pan_law {db, boost}` (default
   `{0, false}`) applies to every channel and buss. A track's optional
   `pan_law` overrides it; absent means "follow the project".
3. **Pan modes.** `track.pan_mode` is one of:
   - `pan` (the default): the left input goes to the left output and the
     right input to the right, scaled by `f_L(pan)` and `f_R(pan)`. This
     balances stereo content and pans mono content (L = R).
   - `dual`: each input is panned as a mono source, the left input to
     `pan_left` (default −100) and the right input to `pan_right` (default
     100), and the two are summed. This places, narrows, widens or swaps
     the image without discarding either side. Two inputs at the same
     position sum, so the law governs that sum (at 0 dB, correlated
     content centred in dual mode is 6 dB hotter).
   - `width`: mid/side width, then pan. With M = (L + R)/2 and
     S = (L − R)/2, the image becomes L' = M + w'·S and R' = M − w'·S, with
     w' = `width`/100 · (1 − |pan|/100). It is then panned like `pan` mode.
     Width 100% leaves the image as it is, 0% is mono (the average, so
     mono content keeps its level) and −100% swaps the sides. The
     (1 − |pan|) factor narrows the image as it moves out, so a hard pan is
     the mono image at that speaker and no side is lost.
   Each mode is a 2×2 matrix from the strip's inputs to its outputs,
   computed only when a control changes. Its four coefficients ramp
   linearly over the smoothing time, like the fader; under automation they
   follow it sample by sample. `pan`, `width`, `pan_left` and `pan_right`
   are mix values (ADR-0060): moving them does not recompile the plan.
   Changing the mode or the law does. `pan` automation drives `pan` in the
   `pan` and `width` modes and does nothing in `dual`. Channels and busses
   also have continuous `width`, `pan_left` and `pan_right` envelopes
   (−100..100 percent) and `pan_boost` (0..1, on at ≥0.5). An active
   nonempty `pan_boost` envelope overrides the effective law's boost per
   sample, whether that law is the strip's or the project's. Masters refuse
   these targets. Following sends and keys keep private copies of the curves
   and their write holds (ADR-0061). All four targets record in touch, latch,
   latch_preview and write; setting `pan_law`, including null, records boost
   when its effective value changes.
   MIDI learn accepts the three positions as continuous pan-shaped controls
   with soft takeover, and `pan_boost` as a toggle. The `pan` group link and
   selection drags carry all four positions by relative percentage-point
   deltas, clamped to −100..100; mode and law (including null) copy absolutely.
   Masters are skipped for these new controls.
4. **The law applies once per strip**, on channels and busses alike: the
   pan matrix and fader act before the strip's C9 Channel encode. The
   C9 Buss has no pan stage. Masters keep their balance stage (ADR-0019 §5),
   which is the 0 dB law in `pan` mode; they take no mode or law. Take pan
   is a balance trim (the 0 dB law) whatever the strip's law, so a centred
   take is unity and the law is not applied twice.
5. **Sends.** A send that follows its strip's pan uses the strip's mode,
   positions and law, so a dual-panned or narrowed image reaches the send
   as the strip shapes it. A send with its own pan uses `pan` mode with that
   position and the source strip's law. Key routes and rack inputs follow
   the same rule.
6. **Unity Console 9 staging.** The engine runs `libs/console9` in its new
   `Staging::Unity` (the default `Staging::Native` stays the faithful port
   that the null tests compare with the reference):
   - the C9 Channel drops its pan stage and the ×0.7639 trim, so the encode
     sees `x · f(2B) · 0.618` (2.34 dB more drive than Airwindows' own
     staging);
   - the C9 Buss drops its pan stage and its ×2 becomes ×φ (1.6180339887),
     the inverse of the curve pair's slope, so an encode → decode pair is
     unity for small signals.
   A hop costs nothing, so a centred source at 0 dB arrives at the master at
   its own level through any number of busses under the default 0 dB law.
   Under a cut law each strip's centred pan still cuts by the law, so a
   channel → buss → master path under −3 dB arrives 6.02 dB down, as in
   REAPER.
7. **Console 9 off** stays level-matched: the channel stage is
   `x · pan matrix · f(2B)` and the buss stage passes its input unchanged.
   A centred 0 dB chain under the 0 dB law is bit-transparent.
8. **No migration.** Existing projects play louder: by 3.5 dB for a
   channel straight into a master, and by about 6.5 dB for every buss on the
   way. Writers emit `supple.project/2.9`. An older application ignores the
   new strip fields with warnings and cannot use the new MIDI bindings;
   builds without the new envelope targets refuse projects that use them.

## Consequences

- Under the default 0 dB law gain staging no longer depends on routing
  depth, no pan position raises either side above the strip's own level,
  and the centre costs nothing. A cut law still costs its centre cut at
  every strip a centred signal crosses; that is the law's job, and a project
  that wants it chooses it.
- Boost can raise a hard-panned signal by up to 6 dB, so a source
  recorded near 0 dBFS can clip a buss's encode. That is why boost is off
  by default.
- `dual` and `width` cost one extra multiply-add per channel per sample;
  `pan` mode costs what it did.
- Pan automation and MIDI learn use the same controls as the GUI. A law's
  dB value and the mode remain discrete edits that compile a plan; automation
  changes positions and compensation sample by sample without compilation.
