# Spec: core-analysis — the perception layer

**Status:** Implemented. Interface changes go through an ADR.
**Libraries:** `libs/core-analysis` (`supple::analysis`), with the DSP pieces
it needs in `libs/core-dsp` (`supple::dsp`) and file I/O in
`libs/audio-file` (`supple::audio`).
**Principle 8:** agents perceive audio through compact, deterministic DSP
descriptors computed at full resolution — never by an audio model
"listening", never as raw FFT dumps.

## 0. Common rules

- Input is planar float audio at any sample rate from 22.05 to 192 kHz,
  mono, stereo or 5.0/5.1 (order L R C LFE Ls Rs; LFE excluded from
  loudness per BS.1770).
- All analysis is **deterministic**: identical input → bit-identical report
  on the same build; the project compiles with `-ffp-contract=off` and no
  fast-math so Clang and GCC agree to within last-bit differences, and
  summary values (rounded, see §8) agree exactly.
- Internal math in double. Offline functions may allocate. The streaming
  `LoudnessMeter` (§1.3) is RT-safe after `prepare`.
- "Mono downmix" below means `(L+R)/2` for stereo, the channel itself for
  mono, and the BS.1770 weighted sum for surround.
- Silence gating for descriptor statistics: frames whose downmix RMS is below
  −70 dBFS are excluded from summary statistics (they still count for
  duration).

```cpp
namespace supple::analysis {
enum class Layout : std::uint8_t { Mono, Stereo, Surround50, Surround51 };
struct Signal {
    std::span<const float* const> channels;   // planar
    std::int64_t frames = 0;
    double sampleRate = 48000.0;
    Layout layout = Layout::Stereo;
};
}
```

## 1. Loudness (ITU-R BS.1770-5, EBU R128, EBU Tech 3341/3342)

### 1.1 Definitions

- **K-weighting:** two cascaded biquads, a high-shelf pre-filter and the RLB
  high-pass, derived for the actual sample rate by bilinear transform of the
  BS.1770 analog prototype (shelf f0 = 1681.974450955533 Hz,
  gain = 3.999843853973347 dB, Q = 0.7071752369554196; high-pass
  f0 = 38.13547087602444 Hz, Q = 0.5003270373238773). At 48 kHz the
  coefficients must match BS.1770's published values to 1e-9.
- **Channel weights:** L, R, C 1.0; Ls, Rs 1.41; LFE excluded.
- **Block loudness:** `L = −0.691 + 10·log10(Σ_i G_i · z_i)` with `z_i` the
  mean square of K-weighted channel i over the block.
- **Momentary** (M): 400 ms rectangular window; **short-term** (S): 3 s;
  both ungated, evaluated every 100 ms (10 Hz) from the moment the window is
  first full.
- **Integrated** (I): 400 ms gating blocks with 75 % overlap (100 ms hop)
  from the start; incomplete final block discarded; absolute gate −70 LUFS;
  relative gate −10 LU below the absolute-gated loudness; I = loudness of the
  mean of `z` over blocks passing both gates. No blocks → `-inf` (reported
  as `null` with a note).
- **Loudness range** (LRA, Tech 3342): short-term values at 10 Hz;
  absolute gate −70 LUFS; relative gate −20 LU below the energy mean of the
  absolute-gated values; LRA = P95 − P10 using Tech 3342's percentile rule
  (sorted ascending, index `round((n−1)·p/100)` zero-based).
- **True peak:** oversample each channel ×4 for fs ≤ 48 kHz, ×2 for
  fs ≤ 96 kHz, ×1 above, with a linear-phase polyphase FIR interpolator
  (document its design: taps, window, passband edge), take the max absolute
  value; report dBTP. **Sample peak** dBFS alongside.
- Max momentary, max short-term, PLR = true peak (dBTP) − I.

### 1.2 Validation (required tests)

Generate every synthesizable EBU minimum-requirements signal **in the test
code** at 48 kHz (and cases 1, 3, 15–19 also at 44.1 and 96 kHz), and assert
the expected responses:

Tech 3341 Table 1 (1 kHz sine applied in phase to both channels, levels are
per-channel peak):

| # | Signal | Expect |
|---|---|---|
| 1 | −23.0 dBFS, 20 s | M, S, I = −23.0 ±0.1 |
| 2 | −33.0 dBFS, 20 s | M, S, I = −33.0 ±0.1 |
| 3 | 10 s −36; 60 s −23; 10 s −36 | I = −23.0 ±0.1 |
| 4 | 10 s −72; 10 s −36; 60 s −23; 10 s −36; 10 s −72 | I = −23.0 ±0.1 |
| 5 | 20 s −26; 20.1 s −20; 20 s −26 | I = −23.0 ±0.1 |
| 6 | 5.0: L, R −28; C −24; Ls, Rs −30; 20 s | I = −23.0 ±0.1 |
| 9 | (1.34 s −20; 1.66 s −30) × 5 | S = −23.0 ±0.1, constant after 3 s |
| 10 | 20 files: i·0.15 s silence; 3 s −23; 1 s silence (i = 0..19) | max S = −23.0 ±0.1 each |
| 12 | (0.18 s −20; 0.22 s −30) × 25 | M = −23.0 ±0.1, constant after 1 s |
| 13 | 20 files: i·20 ms silence; 400 ms −23; 1 s silence | max M = −23.0 ±0.1 each |
| 15 | fs/4, 0.50 FFS, phase 0°, 10 ms fades | TP = −6.0 +0.2/−0.4 dBTP |
| 16 | fs/4, 0.50 FFS, phase 45° | TP = −6.0 +0.2/−0.4 |
| 17 | fs/6, 0.50 FFS, phase 60° | TP = −6.0 +0.2/−0.4 |
| 18 | fs/8, 0.50 FFS, phase 67.5° | TP = −6.0 +0.2/−0.4 |
| 19 | fs/4, 1.41 FFS, phase 45° | TP = +3.0 +0.2/−0.4 |
| 20–23 | fs/6 0.50 FFS sine containing one period of fs/4 at 1.00 FFS, continuous in phase, synthesized at 4·fs, low-pass filtered and decimated with offset 0/1/2/3 | TP = 0.0 +0.2/−0.4 |

(Cases 7, 8 are authentic programme, 11 and 14 are live-meter variants of 10
and 13; 10/13 max-values are asserted per segment file; for the streaming
meter also assert 11 and 14's successive maxima −38.0 … −19.0.)

Tech 3342 Table 1 (LRA; tones 20 s each, back to back):
1: −20, −30 → 10 ±1 LU; 2: −20, −15 → 5 ±1; 3: −40, −20 → 20 ±1;
4: −50, −35, −20, −35, −50 → 15 ±1.

Also: K-weighting frequency response at 48 kHz within 0.01 dB of the
analog prototype at 30 check frequencies; silence → I null, TP −inf handled.

### 1.3 Streaming meter

```cpp
class LoudnessMeter {
  public:
    enum class Mode { Exact, Histogram };  // Exact stores every block (offline); Histogram: fixed 0.01-LU bins, RT-safe
    void prepare(double sampleRate, Layout, Mode, double maxSeconds = 0);  // allocates
    void reset();                                                          // RT-safe
    void process(std::span<const float* const> channels, int frames) SUPPLE_RT;
    double momentary() const; double shortTerm() const;       // latest, LUFS (-inf if not yet full)
    double integrated() const; double loudnessRange() const;  // gated
    double maxMomentary() const; double maxShortTerm() const;
    double truePeak() const;                                  // dBTP, max so far
};
```

Histogram mode must agree with Exact mode within 0.02 LU on every §1.2 case.

## 2. Spectral descriptors (mono downmix)

STFT: Hann window, N = power of two nearest to 0.0427·fs (2048 at 48 kHz),
hop N/4, zero-padded final frame. Magnitude `M[k]`, power `P[k] = M²`,
bin frequency `f_k = k·fs/N`, k = 1 .. N/2 (DC excluded).

- **Centroid** (Hz): `Σ f_k·M[k] / Σ M[k]`.
- **Rolloff85** (Hz): smallest `f_k` with cumulative power ≥ 85 % of total.
- **Flatness:** geometric mean / arithmetic mean of `P[k]` over 30 Hz–16 kHz
  (0 = tonal, 1 = white), with `P` floored at 1e-20.
- **Flux:** frames normalized to unit L2 norm; `flux_t = ||max(0, N_t − N_{t−1})||₂`.
- Per-descriptor summary over non-silent frames: mean, median, p10, p90.
- **Named bands** (power from the long-term average power spectrum):
  `sub` 20–60 Hz, `low` 60–250, `low_mid` 250–500, `mid` 500–2 k,
  `high_mid` 2 k–4 k, `presence` 4 k–6 k, `air` 6 k–20 k (clipped at fs/2).
  Report each band's level in dB relative to the total power in 20 Hz–20 kHz
  (so they read as "−8 dB of the total") and its share in %.
- **Long-term spectrum:** average power over non-silent frames aggregated
  into the 31 ISO 1/3-octave bands 20 Hz–20 kHz (band edges at
  `fc·2^(±1/6)`), in dBFS-scaled power (a full-scale sine lands at 0 dB in its
  band). **Tilt:** least-squares slope of the 1/3-octave levels versus
  log2(frequency) over 100 Hz–10 kHz, in dB/octave.

## 3. Harmonic descriptors (mono downmix)

- **f0:** YIN (cumulative mean normalized difference, threshold 0.10,
  parabolic interpolation), frame 4096 at 48 kHz (scale with fs), hop
  10 ms, range 30–4000 Hz (overridable). Confidence = 1 − d′ at the chosen
  lag; a frame is voiced when confidence ≥ 0.85, not silent and not constant
  (its RMS about its mean is at least the silence floor and 1e-4 of its RMS;
  DC has no period).
- Summary: `pitched` (voiced ≥ 50 % of non-silent frames), median f0 (Hz),
  nearest note name with cents (`A3+4c`), f0 stability (std in cents over
  voiced frames), voiced %.
- **Harmonics:** for voiced frames, spectrum from a Hann window of 4 periods
  (min 2048 samples); for k = 1..16, the peak within ±3 % of k·f0 with
  parabolic interpolation; amplitude in dB relative to k = 1; median over
  voiced frames. Summary returns k = 1..8; full returns 16.
- **Inharmonicity:** amplitude-weighted mean of `|f_k − k·f0| / (k·f0)` over
  detected partials k ≥ 2 (0 = perfectly harmonic).
- **HNR** (dB): per voiced frame `10·log10(r / (1 − r))` with `r` the
  normalized autocorrelation at the detected period, clamped to
  [1e-6, 1 − 1e-6]; median over voiced frames.
- Unpitched material returns `pitched: false` and omits the pitch-dependent
  values (no invented numbers).

## 4. Envelope and dynamics

- **Envelope:** RMS of the mono downmix over 5 ms windows, 1 ms hop.
- **Attack** (ms): time from first reaching 10 % of the envelope maximum to
  first reaching 90 % of it.
- **Decay20** (ms): time from the envelope maximum until it first falls 20 dB
  below it (null if never).
- **Release** (ms, only when `noteOffSeconds` is given): time from note-off
  until the envelope first falls 60 dB below its level at note-off,
  extrapolated linearly in dB from the −10..−30 dB portion if the signal ends
  first (flagged `extrapolated: true`); **sustain** (dB rel. peak): median
  envelope between attack end and note-off.
- **Crest factor** (dB): sample peak / RMS over the whole signal.
- **PLR** (LU): from §1.
- **Onsets / transient density:** onset function = positive flux of
  `log(1 + 100·M)` spectra (N = 1024 at 48 kHz, hop N/4); peaks that are
  local maxima over ±3 frames, exceed the moving median over ±8 frames by
  0.1 × the global max, and are ≥ 30 ms apart. Report onsets/second and (full
  detail) onset times.

## 5. Stereo (stereo input only)

- **Correlation:** Pearson correlation of L and R over the whole signal;
  also over 100 ms windows (non-silent) → mean and p5.
- **Width:** `E_side / (E_mid + E_side)` with mid = (L+R)/2, side = (L−R)/2:
  0 mono, 0.5 uncorrelated, 1 anti-phase. Overall, and per named band (from
  STFT mid/side power), because "the low end is wide" is a mixing finding.
- **Balance** (dB): `10·log10(E_R / E_L)` (positive = right louder).
- **Mono loss** (LU): integrated loudness of the stereo signal minus that of
  the mono downmix fed to both channels (0 for identical channels, ≈ 3 for
  uncorrelated). A downmix that cancels below the −70 LUFS absolute gate is
  measured against −70; when the stereo loudness itself is not measurable
  (under one 400 ms block) mono loss is `null` (and absent from flattened
values). The engine's
  and the sandbox's commands analyze mono, stereo, 5.0 and 5.1 audio by
  channel count (WAV order L R C [LFE] Ls Rs) and refuse other counts with
  `analysis.layout`.

## 6. Comparison, profiles and masking

```cpp
struct DescriptorDelta { std::string key; double reference, test, delta; std::string unit; bool notable; };
struct Comparison { std::vector<DescriptorDelta> deltas; std::vector<double> ltasDeltaDb; /* 31 bands */ double ltasRmsDb; };
Comparison compare(const Report& reference, const Report& test);
```

Keys are dotted report paths (`loudness.integrated_lufs`,
`spectral.centroid_hz.median`, `spectral.bands.low.level_db`, …). `notable`
thresholds: loudness 1 LU; true peak 0.5 dB; centroid / rolloff 10 %;
band levels 1.5 dB; tilt 1 dB/oct; flatness 0.05; correlation 0.1; width 0.05;
attack/decay 20 %; f0 10 cents; harmonics 3 dB. `ltasRmsDb` is the RMS of the
1/3-octave differences over bands active on either side (> −100 dB).
Inactive sides are floored at −100 dB. The mean level offset is estimated
from bands active on both sides and removed; with no shared active bands the
offset is zero. A pure gain change retains zero timbre distance, while silence
against a non-silent signal remains a large mismatch.

**Target profiles** (for agents and the optimizer): a JSON document
`supple.analysis-profile/1.0` with `targets: { "<key>": {"target": x, "tolerance": t, "weight": w} }`
or `{"min": a, "max": b}`, plus optional `ltas` (31 target levels, relative).
`evaluate(report, profile)` returns per-target pass/fail and normalized
distance `|x − target| / tolerance`, and a weighted total loss. The weighted
mean rescales weights before accumulation so large finite weights cannot
overflow their sum or weighted products.

**Masking** (masker A, e.g. kick; maskee B, e.g. bass):

```cpp
struct MaskingBand { double lowHz, highHz; double maskedPct; double meanExcessDb; };
struct MaskingReport { std::vector<MaskingBand> bands; double maskedEnergyPct; std::vector<int> worst; };
Result<MaskingReport> masking(const Signal& masker, const Signal& maskee);
```

Frames as in §2 on each mono downmix (same length required; the shorter is
zero-padded). Power per critical band (24 Bark bands, Zwicker edges). The
masker's band powers (dB) are spread with the Schroeder spreading function
`15.81 + 7.5(Δz + 0.474) − 17.5·sqrt(1 + (Δz + 0.474)²)` dB and lowered by a
fixed 10 dB offset to form the masking threshold. A maskee band in a frame is
**masked** when its power is below that threshold and the maskee band is
above −70 dBFS. Report per band the % of the maskee's active frames masked
and the mean amount by which the threshold exceeds the maskee (dB), the % of
maskee energy masked overall, and the three worst bands. This is an estimate
for corrective mixing decisions, not a psychoacoustic model of record; the
docs must say so.

## 7. Report and API

```cpp
enum class Detail { Summary, Standard, Full };
struct Options {
    bool loudness = true, spectral = true, harmonic = true, envelope = true, stereo = true;
    std::optional<double> noteOffSeconds;
    std::optional<std::pair<double, double>> f0RangeHz;
};
struct Report { /* all values above; optional parts empty when not computed or not applicable */ };
Result<Report> analyze(const Signal&, const Options& = {});
json::Value toJson(const Report&, Detail = Detail::Summary);
std::string summarize(const Report&);  // one paragraph of prose, ≤ 80 words
```

`toJson` writes `"schema": "supple.analysis-report/1.0"`, `source`
(duration s, sample rate, channels, layout), then the sections. Summary
detail: loudness (I, LRA, max M, max S, TP, sample peak, PLR); spectral
medians + tilt + band levels; harmonic summary with 8 harmonics; envelope
scalars; stereo scalars. Standard adds p10/p90, band widths and shares, LTAS.
Full adds time series (M/S at 10 Hz, descriptor frames decimated to ≤ 200
points, onset times, f0 track). A Summary report of a typical 4 s stereo
render must be ≤ 400 tokens (≈ 1600 characters of compact JSON) — test it.

When integrated loudness is unavailable, `summarize` says "Too short to
measure loudness" for buffers shorter than 400 ms with a finite sample peak.
Silent buffers still say "Silent".

## 8. Rounding

Report values are rounded for readability and exact reproducibility: LUFS,
LU, dB, dBTP to 0.1; Hz to 3 significant digits (minimum 1 Hz resolution);
ms to 3 significant digits; ratios/fractions (flatness, correlation, width,
inharmonicity, flux) to 3 decimals; % to 0.1. Full-detail time series use
the same rules. Internal comparisons (`compare`, `evaluate`) use unrounded
values.

## 9. Supporting libraries

**core-dsp** (initial scope; it grew with the plugins): `Biquad` (TDF-II,
double, with RBJ-cookbook designs and bilinear-from-analog helpers),
`PolyphaseUpsampler` (for true peak and oversampling), windows (Hann,
Blackman-Harris, Kaiser), `Fft`, denormal guard (FTZ/DAZ scope), `OnePole`
smoother. Filter, upsampler and smoother processing and the denormal guard
are RT-safe after setup. `Fft::forward` and `inverse` use pocketfft and
allocate scratch; they and arbitrary-rate `resample` are offline APIs.
`Fft::forwardRt` is allocation-free for power-of-two sizes after `prepare`.

**audio-file:** `Result<AudioData> readAudio(path)` (WAV all PCM/float
formats via dr_wav, FLAC via dr_flac, MP3 via dr_mp3) → planar float +
sample rate + channel count; `Status writeWav(path, data, format)` (16/24-bit
PCM with TPDF dither — seeded — or 32-bit float). Round-trip tests.
`inspect(path)` reads container metadata without decoding PCM (for WAV also
the stored bits per sample and whether they are IEEE float). PCM and
IEEE-float WAV headers, including those EXTENSIBLE subformats, must have
`blockAlign` equal to channels times bytes per sample (bit depth rounded up
to whole bytes). FLAC inspection requires a complete, exactly 34-byte
STREAMINFO with a positive sample rate. Violations return `audio.format`.

## 10. Tests and docs

Beyond §1.2: each descriptor against synthetic signals with known answers
(sine centroid ≈ its frequency; white noise flatness ≈ 1 and tilt ≈ 0 in
power-per-Hz terms — document what tilt pink noise gives; sawtooth harmonics
≈ −20·log10(k) dB; a detuned inharmonic partial set with known inharmonicity;
exponential decays with known decay20; L = R width 0, independent noise width
≈ 0.5, anti-phase width 1; balance of a panned sine; masking of a tone by a
louder tone in the same band vs a distant band). Example reports for three
signals (a synth pluck, a pad, a drum loop — generated by the test from
simple synthesis code) committed to `docs/specs/examples/analysis/`.

## Implementation notes

- The meter publishes momentary and short-term values at 10 Hz, while tracking
  their maxima on every sample once the full window exists. This resolves the
  offset-file cases 10/11 and 13/14: a 400 ms tone starting 20 ms after a
  100 ms reporting tick otherwise occupies at most 380 ms of any reported
  400 ms window, producing about −0.22 LU below the stated maximum.
- True-peak interpolation uses a 32-input-tap Kaiser FIR (β = 8.6) per phase,
  sinc cutoff at 0.94 of input Nyquist. It is causal with 15.5 samples of
  input-rate group delay; the meter takes the maximum across all phases and
  channels. For fs ≤ 48 kHz the factor is 4, ≤ 96 kHz is 2, above is 1.
- Arbitrary-rate resampling uses at least 64 input taps, scaling inversely
  with the downsampling ratio (capped at 4096), 2048 fractional phases, Kaiser
  β = 10.5, and a sinc cutoff of 0.90 times the lower Nyquist. At 48 → 24 kHz,
  the measured peak ripple over 100 Hz–8 kHz is 0.0061 dB and the 18 kHz
  output is −123.3 dB. At 192 → 24 kHz, the 8 kHz amplitude error is
  +0.0001 dB and the 40 kHz output is −132.3 dB. Tests exclude 100 edge samples.
- `Fft::forward` and `inverse` delegate to pocketfft for arbitrary sizes and
  are offline APIs because pocketfft allocates transform scratch per call.
  `Fft::forwardRt` uses a prepared radix-2 plan and preallocated scratch for
  RT processing;
  spectral analysis uses power-of-two sizes.

## Implementation notes (2026-09-26, descriptors and reports)

- **Flatness of white noise** is ≈ e^−γ ≈ 0.56 for a raw periodogram
  (exponentially distributed bin powers), not 1; tests assert > 0.5.
- **Tilt** is the slope of 1/3-octave *band power*: white noise reads about
  +3 dB/oct, pink noise about 0 dB/oct.
- **Band power** integrates the long-term spectrum with bins as intervals
  (fractional overlap), so narrow low bands still get energy; with the
  2048-point frame a low sine spreads over neighbouring 1/3-octave bands.
- **Onsets** additionally require the frame energy to rise by ≥ 6 dB over the
  frames before the peak; without it, decays and the beating of detuned
  partials (pads) produced false onsets.
- **YIN** computes the difference function through an FFT cross-correlation
  and running energies (same values as the direct sum).
- **Harmonics** use up to 64 evenly spaced voiced frames; a partial counts
  towards inharmonicity when it is within 60 dB of the fundamental.
- **Rounding:** values are rounded by scaling with a power of ten, so JSON
  prints the decimal value ("-9.2"); non-finite values are `null` (e.g. LRA
  of a render shorter than 3 s).
- **Profiles:** a key known to the report vocabulary but absent from this
  report (f0 of unpitched audio) fails with distance 10; an unknown key is an
  error with the closest key as hint. The `ltas` target counts 3 dB RMS
  (level removed) as one tolerance.
- **Masking** power-sums the spread masker contributions before the 10 dB
  offset. Worst bands are ranked by masked maskee energy.
- Example reports: `docs/specs/examples/analysis/{pluck,pad,drums}.json`,
  regenerated with `SUPPLE_UPDATE_EXAMPLES=1 core-analysis.tests`.

## 9. Live measurement pieces (removed)

A consumer-thread `LiveMeter` (`analysis/live.h`, 2026-09-29) and a rolling
horizon for `LoudnessMeter` (`prepare`'s `windowSeconds`) served the analyzer
Gauge. Lichen, which replaced it, measures inside its own script, so both
were removed on 2026-10-20; `LoudnessMeter` integrates since reset, as
before them.
