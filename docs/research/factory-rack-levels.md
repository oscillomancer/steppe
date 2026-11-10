# Factory rack patch levels after unity Console 9 staging

Measured 2026-11-10 on Linux, Clang dev, 48 kHz. The before values use the
patches at d8ca522a with the unity Console 9 engine; the after values apply
6.52 dB of final linear attenuation to every patch. The eight existing
+1 dBFS ceiling failures are reproduced before; all patches pass after.

The fixture is `host/engine/tests/test_rack_patches.cpp`, “each loads and
plays under full scale”: notes followed by a chord on the patch track, a
−6 dB saw into every In, 0–16 beats at the default 120 BPM, post-fader track
output. Both sets run `analyze.track`. Its public report rounds peaks to
0.1 dB, so temporary measurement instrumentation also rendered the same
track with `EngineApp::renderMix` (`applyMasterPlayrate = false`, track
`tr_1`, 0–8 seconds, post-fader) and measured the unrounded sample maximum.
Original presets were loaded by path from a temporary copy of the baseline
files; the updated presets were loaded by factory name. Identical seeds,
module order and settings keep the renders deterministic.

All values below are sample peaks in dBFS. Every difference is within
0.0001 dB of −6.52 dB, comfortably inside the requested ±0.05 dB. No patch
requires a feedback or level-dependent exception: attenuation is after
those stages. Existing nonlinear drive and modulation depths are untouched.

| Patch | Before | After | Difference |
|---|---:|---:|---:|
| Acid Bass | -3.5524 | -10.0724 | -6.5200 |
| Arp Pluck | -1.4518 | -7.9718 | -6.5200 |
| Drum Kit | -4.8992 | -11.4192 | -6.5200 |
| Euclid Kit | 1.6050 | -4.9150 | -6.5200 |
| FM Bass | 0.1795 | -6.3405 | -6.5200 |
| FM Keys | -0.8346 | -7.3546 | -6.5200 |
| Freeze Pad | -0.5649 | -7.0849 | -6.5200 |
| Glitch Stutter | -4.6855 | -11.2055 | -6.5200 |
| Granular Drone | 0.0805 | -6.4395 | -6.5200 |
| Growing Tune | -2.2628 | -8.7828 | -6.5200 |
| Init Voice | 0.5862 | -5.9338 | -6.5200 |
| One-Finger Chords | 2.5267 | -3.9933 | -6.5200 |
| Rain Bells | -2.8242 | -9.3442 | -6.5200 |
| Ratchet Lead | 2.2164 | -4.3036 | -6.5200 |
| Reese Bass | -11.2761 | -17.7961 | -6.5200 |
| Register Over Chords | 1.1153 | -5.4047 | -6.5200 |
| Resonator Bells | -1.2309 | -7.7509 | -6.5200 |
| Round | -0.5505 | -7.0705 | -6.5200 |
| Shimmer Space | 1.3677 | -5.1523 | -6.5200 |
| Song Machine | -10.3723 | -16.8923 | -6.5200 |
| Strange Attractor | -2.5888 | -9.1088 | -6.5200 |
| String Pluck | 2.0157 | -4.5043 | -6.5200 |
| Sync Lead | -3.7954 | -10.3154 | -6.5200 |
| Tape Warmth | -7.5124 | -14.0324 | -6.5200 |
| Vocoder | 4.1409 | -2.3791 | -6.5200 |
| Wavetable Pad | 1.3074 | -5.2126 | -6.5200 |
