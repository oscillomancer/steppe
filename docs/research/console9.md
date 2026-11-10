# Research: Airwindows Console 9 (Channel and Buss)

**Source.** Chris Johnson's Airwindows repository,
<https://github.com/airwindows/airwindows>, commit `d22a25b7f7c0` (2026-09-18),
MIT License, © 2018 Chris Johnson. Read: `plugins/LinuxVST/src/Console9Channel/`,
`plugins/LinuxVST/src/Console9Buss/` (stereo, VST2) and
`plugins/MacSignedAU/Console9ChannelMono/`, `Console9BussMono/` (the AU mono
kernels). The reference copy used by our null test lives in
`third_party/airwindows-console9/` (test-only, never shipped; ADR-0019).

## What the two stages do

Console 9 is a matched pair of memoryless waveshapers with gain and pan
around them. Each plug-in has exactly two parameters, `A` = **Pan** and
`B` = **Fader**, both 0..1 with default 0.5. There is no filter, no state
other than the smoothing endpoints and the dither generators, and **no
sample-rate dependence** (nothing reads the rate).

Per stereo sample, **Console 9 Channel** (the encode):

1. Denormal guard: if |x| < 1.18e-23, x is *replaced* by `fpd × 1.18e-17`
   (the channel's current dither state, a uint32: a positive value up to
   ≈ 5e-8, −146 dBFS). Silence therefore becomes very quiet positive noise.
2. Pan: `angle = A × 1.57079633` (sic: that constant, not π/2 exactly);
   `gainR = sin(angle)`, `gainL = sin(1.57079633 − angle)` — a constant-power
   sin/cos law, −3.01 dB on each side at the centre. It is applied to a
   stereo pair as a balance (L scaled by gainL, R by gainR).
3. Fader: `t = 2B`; `f(t) = t²` for t > 1, `1 − (1 − t)²` for t < 1 (t at 1);
   gain = `f(t) × 0.763932022500211`. f is 0 at B = 0, 1 at B = 0.5 (the
   native unity), 4 at B = 1 (+12.04 dB).
4. Encode: `u = x × 0.618033988749895`, then
   `E(u) = sign(u) · (1 − (1 − |u|)^1.618033988749895)` for |u| < 1 and
   ±1 beyond. E is concave (compressive); its slope at 0 is φ, so with the
   ×1/φ pre-scale the small-signal gain of step 4 is exactly 1.
5. 32-bit float dither: `frexpf(float(x), &e)`, xorshift32 step of `fpd`,
   `x += (fpd − 0x7fffffff) × 5.5e-36 × 2^(e+62)` — about ±1 float ulp of x.

**Console 9 Buss** (the decode): the same denormal guard; clip to ±1; the
exact inverse curve `D(v) = sign(v) · (1 − (1 − |v|)^0.6180339887498949)`
(slope 1/φ at 0); then the same pan law and fader shape with ×2.0 instead of
×0.7639; then the same dither.

Parameter smoothing: at each host block the previous and new A/B values are
interpolated linearly across the block (`temp = remaining/blockLength`), so
automation ramps depend on the host's block size. Dither state `fpdL/fpdR`
is seeded from `rand()` in the constructor (not stored in the plug-in's
chunk).

The mono AU kernels (`Console9ChannelMono`, `Console9BussMono`) process each
channel independently with the **fader only** — no pan stage — and are
otherwise identical (encode gain ×0.7639, decode gain ×2).

## Consequences

- **One channel into one buss is linear.** D(E(u)) = u for |u| < 1, so a
  single encoded signal decoded by a buss comes back as a pure gain (plus
  dither); the curves only clip beyond |u| ≥ 1 (an input about 6.5 dB over
  full scale at unity). The Console 9 character is in the **summing**: a buss
  decodes the *sum of encoded* signals, D(Σ E(uᵢ)) ≠ Σ uᵢ — coincident
  encoded peaks expand relative to linear summing before the decode clips
  (D is convex for positive inputs).
- **Gain staging at native unity** (both faders B = 0.5, both pans centred):
  channel = 0.7639 × 0.7071 = 0.540 (−5.35 dB); buss = 0.618 × 2 × 0.7071 =
  0.874 (−1.17 dB); channel → buss small-signal gain = 0.472 (−6.52 dB).
  With clean digital summing the same strip is 0 dB (plus the pan law).
- **Silence is not bit-exact**: every Console 9 stage turns digital silence
  into seeded noise near −150 dBFS with a small positive bias, and adds
  ±1 ulp dither to everything. Deterministic once seeded.
- **Cost per stereo sample**: two `log1p`, two `expm1`, two `frexpf`, two
  `pow(2, n)`, the sin/cos pair and the fader curve (all per sample in the
  reference). `pow(2, n)` for integer n equals `ldexp(1, n)` exactly, and the
  pan/fader terms are constant between parameter changes, so a port can be
  much cheaper while staying bit-identical.

## Mapping to the DAW's semantic units (ADR-0019)

- **Fader (dB)** = `20·log10(f(2B))`: −inf at B = 0, 0 dB at B = 0.5,
  +12.04 dB at B = 1. Inverse: for g = 10^(dB/20) ≥ 1, B = √g / 2; for g < 1,
  B = (1 − √(1 − g)) / 2. The DSP only needs the linear gain g, never B.
- **Pan (%)** p ∈ [−100, 100] ↔ A = (p/100 + 1) / 2 (−100 = hard left).
- Console 9 Buss is held at A = B = 0.5 and never exposed.

*(Amended 2026-11-10, ADR-0066.)* The engine no longer uses the pan stage or
the fixed trims above. It runs both stages in `Staging::Unity`: the strip
pans before the encode with its own pan mode and law, the encode is driven
by `x · f(2B) · 0.618` (no ×0.7639, so 2.34 dB more drive than Airwindows'
own staging), and the decode is followed by ×φ instead of ×2 (and no pan),
so a channel → buss pair is unity for small signals. The port keeps the
reference staging (`Staging::Native`) for the null tests.
