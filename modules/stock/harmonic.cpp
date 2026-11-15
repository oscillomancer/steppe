// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// An additive oscillator, poly: up to thirty-two sine partials, each at its
// own ratio of the pitch and its own level, summed. Partials sets how many
// sound (between whole numbers the last one fades in); Brightness tilts
// their levels from a dark flute (0) through a saw's (50) to a flat buzz
// (100); Odd/Even takes out the even partials (hollow, a clarinet's, at
// -100) or the odd ones above the fundamental (octaves, an organ's, at
// +100). Stretch spreads the partials wider than harmonic (bells, metal) or
// squeezes them closer (gongs, a detuned growl). The comb carves notches
// into the spectrum, one every Comb Spacing partials, as deep as Comb; Comb
// Shift slides them up through the partials, a phaser made of overtones:
// patch a slow ramp into it and they sweep forever. A partial that would
// pass half the sample rate is left out, so it never aliases.
//
// The level stays the same whatever the controls (the partials are scaled
// to the same total power), and their starting phases are spread so the
// peaks stay low. Odd and Even carry the odd and even partials apart
// (the fundamental is odd): their sum is Out, or a stereo pair.

constexpr int kPartials = 32;
constexpr double kC4 = 261.6255653005986;

struct Partials {
    Array<double, kPartials> phase;
    Array<double, kPartials> amp;    // each partial's level now
    Array<double, kPartials> step;   // its change each frame, to the block's target
    Array<double, kPartials> ratio;  // its frequency over the fundamental
};

struct Harmonic : Module {
    static constexpr ModuleInfo info = {
        .name = "Harmonic",
        .id = "com.suppleaudio.rack.harmonic",
        .vendor = "Supple Audio",
        .description =
            "An additive oscillator: up to 32 partials with brightness, odd/even, "
            "stretch and a sweeping spectral comb.",
        .tags = {"oscillator", "additive", "poly"}};

    Param partials = continuous("Partials", 1, 32, 16)
                         .in("Spectrum")
                         .help("How many partials sound; between whole numbers the last fades in.");
    Param brightness =
        percent("Brightness", 50)
            .in("Spectrum")
            .help("Tilts the partials' levels: dark at 0, a saw's at 50, flat and buzzy at 100.");
    Param oddEven = percent("Odd/Even", 0, -100, 100)
                        .in("Spectrum")
                        .help(
                            "Below 0 takes out the even partials (hollow), above 0 the odd ones "
                            "over the fundamental (octaves).");
    Param stretch = percent("Stretch", 0, -100, 100)
                        .in("Spectrum")
                        .help(
                            "Spreads the partials wider than harmonic (bells, metal) or squeezes "
                            "them closer (gongs).");
    Param comb = percent("Comb", 0).in("Comb").help("How deep the comb's notches cut, 0 to none.");
    Param combSpacing =
        continuous("Comb Spacing", 2, 8, 4).in("Comb").help("Partials from one notch to the next.");
    Param combShift = percent("Comb Shift", 0)
                          .in("Comb")
                          .help(
                              "Slides the notches up by a spacing as it goes from 0 to 100: a "
                              "ramp into it sweeps them forever.");
    Param coarse =
        semitones("Coarse", -48, 48, 0).in("Pitch").help("Transposes the pitch in semitones.");
    Param fine = cents("Fine", -100, 100, 0).in("Pitch").help("Detunes the pitch in cents.");
    Param fmDepth = octaves("FM Depth", -4, 4, 1)
                        .in("Pitch")
                        .help("How far the FM input bends the pitch, in octaves at a CV of 1.");

    Input pitch = pitchIn("Pitch").help("1 per octave; 0 is C4. Poly: one voice per channel.");
    Input fm = cvIn("FM").help("Bends the pitch by FM Depth's octaves: vibrato, sweeps.");

    Output out = audioOut("Out").help("All the partials.");
    Output odd = audioOut("Odd").help("The odd partials (the fundamental, the third, ...).");
    Output even = audioOut("Even").help("The even partials (the second, the fourth, ...).");

    Array<Partials, 16> voice;
    int untilPlan = 0;  // frames to the next plan

    void prepare() {
        // Phases spread as n^2 / 64 of a cycle: the partials' peaks seldom meet.
        for (int c = 0; c < 16; ++c)
            for (int n = 0; n < kPartials; ++n) {
                const double k = n + 1.0;
                const double p = k * k / 64.0;
                voice[c].phase[n] = p - floor(p);
                voice[c].amp[n] = 0.0;
                voice[c].step[n] = 0.0;
                voice[c].ratio[n] = k;
            }
        untilPlan = 0;
    }

    // Channel c's ratios, and the levels its partials glide to over the next
    // 32 frames, from the values at frame i.
    void plan(int c, int i, double inc) {
        Partials& v = voice[c];
        const double count = clamp(partials.at(i, c), 1.0, 32.0);
        const double tilt = 2.0 * (1.0 - clamp(brightness.at(i, c), 0.0, 1.0));
        const double oe = clamp(oddEven.at(i, c), -1.0, 1.0);
        const double evenGain = 1.0 + min(oe, 0.0);
        const double oddGain = 1.0 - max(oe, 0.0);
        const double power = 1.0 + 0.3 * clamp(stretch.at(i, c), -1.0, 1.0);
        const double depth = clamp(comb.at(i, c), 0.0, 1.0);
        const double spacing = clamp(combSpacing.at(i, c), 2.0, 8.0);
        const double shift = combShift.at(i, c);
        double total = 0.0;
        for (int n = 0; n < kPartials; ++n) {
            const double k = n + 1.0;
            const double ratio = pow(k, power);
            v.ratio[n] = ratio;
            double a = pow(k, -tilt);
            if (n > 0) a *= n % 2 == 1 ? evenGain : oddGain;
            a *= clamp(count - n, 0.0, 1.0);
            const double tooth = 0.5 + 0.5 * cos(twoPi * (n / spacing - shift - 0.5));
            a *= 1.0 - depth * tooth * tooth;
            // Fade out from 0.4 of the sample rate to 0.45.
            a *= clamp((0.45 - inc * ratio) / 0.05, 0.0, 1.0);
            v.step[n] = a;
            total += a * a;
        }
        const double norm = 0.5 / sqrt(max(total, 1e-12));
        for (int n = 0; n < kPartials; ++n) v.step[n] = (v.step[n] * norm - v.amp[n]) / 32.0;
    }

    // The partials are planned every 32 frames from the start, whatever the
    // host's blocks, so a render does not depend on where they fall.
    void process(int frames) {
        const double sr = sampleRate();
        int from = 0;
        while (from < frames) {
            if (untilPlan == 0) {
                for (int c = 0; c < channels(); ++c) {
                    const double oct0 = pitch[c][from] + coarse.at(from, c) / 12.0 +
                                        fine.at(from, c) / 1200.0 +
                                        fmDepth.at(from, c) * fm[c][from];
                    plan(c, from, kC4 * exp2(clamp(oct0, -14.0, 7.0)) / sr);
                }
                untilPlan = 32;
            }
            const int to = min(frames, from + untilPlan);
            sound(from, to);
            untilPlan -= to - from;
            from = to;
        }
    }

    void sound(int from, int to) {
        const double sr = sampleRate();
        for (int c = 0; c < channels(); ++c) {
            Partials& v = voice[c];
            for (int i = from; i < to; ++i) {
                const double oct = pitch[c][i] + coarse.at(i, c) / 12.0 + fine.at(i, c) / 1200.0 +
                                   fmDepth.at(i, c) * fm[c][i];
                const double inc = kC4 * exp2(clamp(oct, -14.0, 7.0)) / sr;
                double sumOdd = 0.0, sumEven = 0.0;
                for (int n = 0; n < kPartials; ++n) {
                    const double a = v.amp[n] + v.step[n];
                    v.amp[n] = a;
                    const double rate = inc * v.ratio[n];
                    const double p = v.phase[n] + rate;
                    v.phase[n] = p - floor(p);
                    // Silent when FM takes it past the fade.
                    if (a == 0.0 || rate >= 0.47) continue;
                    const double y = a * sin(twoPi * v.phase[n]);
                    if (n % 2 == 0)
                        sumOdd += y;
                    else
                        sumEven += y;
                }
                out[c][i] = sumOdd + sumEven;
                odd[c][i] = sumOdd;
                even[c][i] = sumEven;
            }
        }
    }
};
