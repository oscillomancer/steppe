# Reading analysis reports

`analyze.render {render}` or `analyze.track {track}` (or `render.mix` with
`"analyze": "summary"`) returns a
`supple.analysis-report/1.0`: numbers that stand in for your ears. This page
says what each one measures, what normal values look like, and what moves it.
Definitions are exact in `docs/specs/core-analysis.md`; the summary detail is
what you usually get. Field names below are relative to their section
(`loudness`, `spectral`, `harmonic`, `envelope`, `stereo`). Typical values are
heuristics for choosing a starting point, not guaranteed classifications.

Request `detail: "standard"` on `analyze.render` or `analyze.track` for the
statistics, band objects and LTAS described below. In the sandbox, use
`analyze {source: <render id or file>, detail: "standard"}`. Summary detail
reduces centroid/rolloff/flatness/flux to scalar medians and bands to scalar
relative levels: `spectral.bands.low`, not `spectral.bands.low.level_db`.
It omits band widths and LTAS.

## Loudness (EBU R128 / ITU-R BS.1770)

| field | means | typical |
|---|---|---|
| `integrated_lufs` | overall loudness of the whole render | streaming masters −14; a single synth part −20..−14 |
| `max_momentary_lufs` / `max_short_term_lufs` | loudest 400 ms / 3 s | |
| `momentary_lufs` / `short_term_lufs` (`detail: full`) | a value every 100 ms: the 400 ms / 3 s loudness ending there. They start at the first full window (0.4 s, 3 s) and are null before it and in digital silence | compare two renders point by point to see a level move with time |
| `lra_lu` | loudness range (dynamics over time); null for short renders | pads 1–4, drums 4–10 |
| `true_peak_dbtp` | inter-sample peak | keep ≤ −1 dBTP for delivery |
| `plr_lu` | peak-to-loudness ratio: punch vs density | squashed < 8, dynamic > 12 |

Loudness is not quality. Both hosts remove the average level offset when
comparing LTAS shape. The sandbox's `compare` also returns a level-matched
optimizer loss; the DAW's `analyze.compare` returns descriptor differences.

## Spectrum

- `centroid_hz` — the spectrum's centre of mass: **brightness**. Bass
  100–400, piano/pad 500–2 k, bright leads 2–5 k, hats > 6 k. `median` is the
  robust value; `p10`/`p90` show how much it moves.
- `rolloff_hz` — below this frequency sits 85 % of the energy.
- `flatness` — 0 tonal … 1 noise. Saws ≈ 0.01–0.05, noise snares > 0.3.
- `flux` — frame-to-frame spectral change: **movement**. Static pads < 0.01,
  plucks and arpeggios higher.
- `tilt_db_per_oct` — overall slope 100 Hz–10 kHz. Pink noise ≈ −3; a saw
  through a low-pass −9 or steeper; bright/harsh sounds near 0.
- `bands.<name>.level_db` — each band relative to the total power (0 dB = all
  the energy is here): `sub` 20–60 Hz, `low` 60–250, `low_mid` 250–500 (mud),
  `mid` 500–2 k, `high_mid` 2–4 k (harshness), `presence` 4–6 k, `air`
  6–20 k. `share_pct` is the same as a percentage; `width` the band's stereo
  width (wide lows are a mixing problem).
- `ltas_db` — 31 third-octave levels 20 Hz–20 kHz: the long-term spectrum,
  for detailed comparison (`compare` reports its RMS difference).

## Pitch and harmonics (pitched material)

- `pitched`, `f0_hz`, `note` (`G3+4c`), `stability_cents` (vibrato/drift),
  `voiced_pct`.
- `harmonics_db` — harmonics 1..8 (1..16 in full detail) relative to the
  fundamental. A saw falls about 6 dB per octave (h2 ≈ −6, h4 ≈ −12), a square has weak even
  harmonics, a sine has none. Drive raises them; a low-pass cuts the upper
  ones.
- `inharmonicity` — 0 for harmonic sounds; bells, FM with non-integer ratios
  and stiff strings read higher (0.01–0.1+).
- `hnr_db` — harmonics-to-noise ratio: clean tones > 30, breathy/noisy < 10.

Unpitched sounds (drums, noise) retain `pitched: false` and `voiced_pct`,
and omit the remaining harmonic fields.

## Envelope (render one note to measure it)

- `attack_ms` — 10 % → 90 % of the peak. Plucks < 5, pads 200+.
- `decay20_ms` — peak to −20 dB: how fast it fades after the hit.
- `release_ms` (when measurable and the note-off is known) — note-off to
  −60 dB. `release_extrapolated: true` marks an estimate from the decay slope.
- `crest_db` — peak over RMS: transient spikiness.
- `onsets_per_s` — note/transient density.

Multi-note phrases blur envelope numbers; measure envelopes on a single note.

## Stereo

- `width` — 0 mono, ≈ 0.5 uncorrelated, 1 out of phase.
- `correlation` — +1 mono, 0 uncorrelated, negative = phase problems (check
  `mono_loss_lu`: how much quieter the mono fold-down is).
- `balance_db` — positive = right louder.

## Comparing two renders

`analyze.compare {reference, test}` lists the descriptors that changed notably
(score ≥ 1 means beyond the usual threshold for that descriptor), the LTAS
RMS difference in dB (< 1 dB: very close, > 3 dB: clearly different
timbre). Only LTAS has its average level offset removed; notable descriptor
deltas retain loudness changes. There is no removed-gain field. Use it
after every change: it tells you exactly what your change did.

`branch.render-ab {a, b, end_beat}` renders two branch heads over the same
range and returns their render ids with a comparison summary. It does not
switch the active branch. For an exported stem or region, the render result
includes a summary report alongside the file path and render id.
