# Console 9 DSP

`console9` / `supple::console9` implements Airwindows Console 9 Channel
(encode) and Buss (decode), including native fader/pan, the denormal guard,
xorshift streams and float dither. Copyright © 2018 Chris Johnson; MIT,
see `LICENSE`. Design: ADR-0019 and `docs/research/console9.md`.

The shipped library is SDK-free and allocation-free. The original reference
is a separate test-only target, created only with `SUPPLE_BUILD_TESTS=ON`;
no reference objects or test stand-in are linked into `console9`.

```cpp
supple::console9::Channel channel;
channel.reset(supple::console9::seedsFor(projectSeed, stableStageSalt));
channel.set(supple::console9::faderGain(volumeDb),
            supple::console9::nativePan(panPercent), rampSamples);
channel.process(inputs, outputs, frames);
```

`Channel` and `Buss` process stereo pointer pairs (float or double), including
in-place buffers. To pan a mono source, pass its pointer as both inputs; both
are read before writing either output. `MonoChannel` and `MonoBuss` process
one input/output pointer, with the AU mono kernels' fader-only behaviour;
their pan argument is ignored. Double mono processing follows the stereo
reference's double convention. No stage has a sample-rate argument.

`setLinear(true)` selects the level-matched Console 9 off mode (amended
ADR-0019 §6); default is curves on. It keeps the native fader, pan law and
ramps. Channel skips both the ×0.618 pre-scale and encoder curve, retaining
`x * pan * (gain * 0.763932022500211)`. Buss substitutes its small-signal slope
for the decoder: `x * 0.6180339887498949 * pan * (gain * 2.0)`. Mono uses the
same formulas without pan. There is no clipping, denormal replacement,
dither or RNG progress in linear mode; actual tiny inputs are preserved
through ordinary arithmetic and silence stays silent. The nonfinite-input
policy still applies. Gain terms retain the reference's multiplication
order, including float rounding between stages. Mode selection dispatches
once per block to a separate sample loop. It preserves settings and RNG
state; switching/crossfading is a plan-level responsibility, with no internal
mode ramp. There is no separate unity-gain mixer mode.

`setStaging(Staging::Unity)` selects the host's staging (ADR-0066); the
default `Staging::Native` is Airwindows' own and is what the null tests
compare. Unity has no pan stage (the pan argument is ignored: the host pans
before the encode), no ×0.7639 encode trim and ×φ instead of ×2 after the
decode, so a Channel → Buss pair is unity for small signals and the encode
is driven by `x · gain · 0.618`. Linear Unity stages are `x · gain` for the
Channel and `x · gain` for the Buss (its slope and ×φ cancel), so a linear
pair at unity is bit-transparent. Staging keeps settings, ramps and seeds.

`set(g,A,n)` takes linear gain g=f(2B), clamped to 0…4, and native pan A,
clamped to 0…1; n=0 is immediate. `setNative(float B,float A,n)` computes the
native fader using the original promotion/order and constants. Gain and pan
angle ramp linearly for exactly n samples, advancing before each sample;
retargeting starts at the current position, and the final sample snaps to the
requested target. Partitions do not affect ramps or dither. Zero-frame calls
with valid pointers do not advance anything. Calls and parameter edits belong
to the owning audio thread; these objects are not concurrent parameter stores.

`reset(Seeds)` restarts only the random streams, leaving settings and ramps
unchanged. For a deterministic locate, first restore the settings immediately,
then reset from the stage's stored seeds. `seedsFor(projectSeed,salt)` mixes
salt once with SplitMix64, XORs it with the project seed, then takes the low
32 bits of two successive SplitMix64 outputs, clamped to 16386. The caller
chooses a stable salt for strip/stage/send identity; it must not depend on
worker order. Explicit reset seeds below 16386 are also clamped.

`faderGain(db)` maps −infinity to zero, 0 dB to one, and saturates at four
(12.0411998266 dB). `nativeFader(db)` and `faderDb(B)` implement the native
curve and inverse; pan percent maps linearly to A. NaN gain/fader values map
to zero, NaN pan to hard left; infinities clamp to the appropriate endpoint.
Nonfinite audio samples are treated as zero before the denormal guard (an
intentional deviation from upstream NaN propagation).

## Fidelity and intentional differences

Arithmetic order and decimal constants follow the original. Pan/fader terms
are cached when stationary. Dither computes the reference's expression in
long double, adds to the promoted double sample, then rounds back to double
before float output. `ldexp(1.0,e+62)` replaces `pow(2,e+62)`; an exhaustive
float-exponent test proves these values equal, and the reference null test
checks the entire expression. `frexpf` still sees the float-cast sample.
The denormal guard uses the RNG state before that sample's xorshift. Double
I/O advances the RNG but does **not** add dither: upstream comments it out.

The port uses caller-sized per-sample ramps rather than upstream's block-sized
native-parameter ramps. Even static upstream endpoints evaluate
`a*t+a*(1-t)`, which can round differently from a. The production port caches
a; its block-size-1 null test is entirely bit-identical. At larger blocks,
the null test reports every difference and requires exact identity when a
test-only accessor injects those original interpolated coefficients. It
injects only coefficients; all sample arithmetic still uses the shipped
library and is compared with the unmodified reference processor. The same
check covers each encoder and the decoder in the five-channel sum test.

Hard-right A=1 is a special upstream rounding case: nominally zero left pan
can become ±one double ulp. For float I/O, exact zero has `frexpf` exponent
zero, while a tiny nonzero sample has a very negative exponent. Its dither
therefore changes by up to approximately 5.45e−8 (−145.3 dBFS), despite the
coefficient error being tiny. Relative float-ulp counts near this almost-zero
reference output can be enormous; both absolute error and the ordinary
(non-hard-right-left) ulps are reported. All such differences must be
explained exactly by the coefficient-injection check. Digital silence and a
zero fader do not guarantee bit-zero output; preserve upstream's noise and
use thresholds for silence detection.

`console9.tests` is labelled `rt`. It sweeps B=0,.05,…,1 and A=0,.25,…,1;
44100/48000/96000 Hz signal content; float and double; blocks 1/7/64/512/4096;
swept sine and noise −60…+12 dBFS, silence, signed zeros and values on both
sides of the double and float denormal thresholds. Other tests cover sums,
bit-exact ramp splits/retargets in all four stages, mappings/seeds, neutral
0.4721 staging, Unity staging, clipping, reset, nonfinite input, mono/in-place behaviour,
linear ramp length and power-of-two equivalence. AU files are uncompiled
reference text; mono kernels additionally match the stereo hard-left path.

Release benchmark (not a test):

```sh
cmake --preset release
cmake --build --preset release -j6 --target console9-bench
build/release/libs/console9/console9-bench
```

Reports ns per stereo frame and `% of one core = ns * 48000 / 1e7`, for
Channel/Buss, float/double, curves/linear, stationary and changing gain/pan. Timing is an
observation, never a test pass/fail criterion. See `VERIFICATION.md` for the
measured null table, benchmark numbers and commands.
