// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A vocoder: a voice (or drums, or anything) shapes a carrier, so a synth
// chord speaks or sings with the voice's vowels and consonants. Both are
// split into the same bands, from Low to High; each of the voice's bands
// sets, by its level, how loud the carrier's matching band plays. More
// bands are clearer, fewer more robotic; Width widens each band for a
// smoother, breathier sound. Attack and Release set how fast the bands
// follow. Shift moves the carrier's bands up or down against the voice's:
// a smaller or larger throat. Noise adds hiss to the upper bands' carrier,
// so S and T come through even over a carrier without highs. A poly
// carrier (a chord) is summed first. With a full, bright carrier (a saw at
// full scale) the output is at about the voice's level, whatever Bands and
// Width; a dull carrier comes out quieter, as it has less in the bands.

constexpr int kMostBands = 32;
constexpr double kNoiseFromHz = 2500.0;  // Noise feeds the bands above
constexpr double kScale = 10.0;          // the output's gain, to about the voice's level

struct Band {
    Biquad voiceA, voiceB, carrierA, carrierB;  // two band passes each
    double rise = 0.0, level = 0.0;             // the voice band's, followed in two stages
    double centre = 0.0;                        // the voice band's centre, Hz
};

struct Vocoder : Module {
    static constexpr ModuleInfo info = {
        .name = "Vocoder",
        .id = "com.suppleaudio.rack.vocoder",
        .vendor = "Supple Audio",
        .description =
            "A vocoder: 4 to 32 bands, the voice's levels on a carrier's bands; width, "
            "attack, release, formant shift, noise for sibilance.",
        .tags = {"effect", "filter", "voice"}};

    Param bands = integer("Bands", 4, kMostBands, 16)
                      .in("Bands")
                      .help("How many bands: more is clearer, fewer more robotic.");
    Param low = frequency("Low", 60, 1000, 100).in("Bands").help("The lowest band's centre.");
    Param high =
        frequency("High", 2000, 12000, 7000).in("Bands").help("The highest band's centre.");
    Param width =
        percent("Width", 50)
            .in("Bands")
            .help("Each band's width: wider is smoother and breathier, narrower more ringing.");
    Param attack = milliseconds("Attack", 0.5, 100, 4)
                       .in("Follow")
                       .help("How fast a band opens as the voice gets louder in it.");
    Param release = milliseconds("Release", 5, 1000, 40)
                        .in("Follow")
                        .help("How fast it closes: longer smears words into a pad.");
    Param shift = semitones("Shift", -12, 12, 0)
                      .in("Tone")
                      .help(
                          "Moves the carrier's bands against the voice's: up a smaller throat, "
                          "down a larger.");
    Param noise = percent("Noise", 25)
                      .in("Tone")
                      .help("Hiss in the upper bands' carrier, for S, T and breath.");
    Param level = decibels("Level", -24, 24, 0).smoothed(20).in("Tone").help("The gain after.");

    Input voice =
        audioIn("Voice").help("What speaks: a voice, drums, a loop. Poly cables are summed.");
    Input carrier = audioIn("Carrier").help(
        "What it speaks through: a rich synth (saws, a chord). Poly cables are summed.");
    Output out = audioOut("Out").help("The vocoded sound.");

    Array<Band, kMostBands> band;
    Noise hiss;
    SharedArray<float, kMostBands> shown;
    Shared<int> shownCount;

    void prepare() { hiss.seed(seed()); }

    void process(int frames) {
        const double sr = sampleRate();
        const int n = clamp(toInt(bands.value()), 4, kMostBands);
        out.setChannels(1);
        // The bands' centres, evenly spaced in octaves from Low to High; each
        // band's Q its share of those octaves, from half to twice the spacing
        // by Width.
        const double lo = low.value(), hi = max(high.value(), lo * 2.0);
        const double spacing = log2(hi / lo) / (n - 1);
        const double octaves = spacing * (0.5 + 1.5 * clamp(width.value(), 0.0, 1.0));
        const double q = 1.0 / (2.0 * sinh(0.5 * log(2.0) * octaves));
        const double turn = exp2(shift.value() / 12.0);
        const double top = 0.45 * sr;
        for (int k = 0; k < n; ++k) {
            Band& b = band[k];
            b.centre = min(lo * exp2(spacing * k), top);
            const double to = min(b.centre * turn, top);
            b.voiceA.bandpass(b.centre, q);
            b.voiceB.bandpass(b.centre, q);
            b.carrierA.bandpass(to, q);
            b.carrierB.bandpass(to, q);
        }
        const double up = 1.0 - exp(-1.0 / (attack.value() * 0.0005 * sr));
        const double down = 1.0 - exp(-1.0 / (release.value() * 0.0005 * sr));
        const double hissy = clamp(noise.value(), 0.0, 1.0);
        // Narrower bands each catch less of the voice and of the carrier:
        // made up for, so Bands and Width leave the level about where it is.
        const double wide = clamp(width.value(), 0.0, 1.0);
        const double gain = kScale * pow(n / 16.0, 0.75) * pow(10.0, -0.525 * (wide - 0.5));
        for (int i = 0; i < frames; ++i) {
            double v = 0.0, c = 0.0;
            for (int ch = 0; ch < max(voice.channels(), 1); ++ch) v += voice[ch][i];
            for (int ch = 0; ch < max(carrier.channels(), 1); ++ch) c += carrier[ch][i];
            const double h = hiss.white();
            double y = 0.0;
            for (int k = 0; k < n; ++k) {
                Band& b = band[k];
                const double heard = abs(b.voiceB.process(b.voiceA.process(v)));
                // Two stages, each half the time, to smooth out the ripple of
                // the rectified band: less grit, cleaner bands.
                const double r0 = b.rise;
                b.rise = r0 + (heard > r0 ? up : down) * (heard - r0);
                const double l0 = b.level;
                b.level = l0 + (b.rise > l0 ? up : down) * (b.rise - l0);
                const double feed = b.centre >= kNoiseFromHz ? c + hissy * h : c;
                const double played = b.carrierB.process(b.carrierA.process(feed));
                y += b.level * played;
            }
            out[0][i] = y * gain * pow(10.0, level.at(i, 0) / 20.0);
        }
        for (int k = 0; k < kMostBands; ++k)
            shown[k] = k < n ? static_cast<float>(band[k].level) : 0.0f;
        shownCount.set(n);
    }

    void panel(Panel& p) {
        p.size(196, 316);
        p.display(8, 28, 180, 64);
        p.knob(bands, 32, 128);
        p.knob(low, 76, 128);
        p.knob(high, 120, 128);
        p.knob(width, 164, 128);
        p.knob(attack, 32, 184);
        p.knob(release, 76, 184);
        p.knob(shift, 120, 184);
        p.knob(noise, 164, 184);
        p.knob(level, 98, 240);
        p.input(voice, 32, 290);
        p.input(carrier, 76, 290);
        p.output(out, 164, 290);
    }

    void draw(Canvas& g) {
        const double x0 = 8.0, y0 = 28.0, w = 180.0, h = 64.0;
        g.well(x0, y0, w, h);
        int n = shownCount.get();
        const bool live = n > 0;
        if (!live) n = clamp(toInt(bands.value()), 4, kMostBands);
        const double gap = 1.0, bar = (w - 12.0) / n;
        for (int k = 0; k < n; ++k) {
            const double x = x0 + 6.0 + k * bar;
            g.fillStyle(Theme::line);
            g.fillRect(x, y0 + 6.0, bar - gap, h - 12.0);
            if (live) {
                const double v = clamp(
                    1.0 + 20.0 * log10(static_cast<double>(shown[k]) + 1e-6) / 60.0, 0.0, 1.0);
                g.fillStyle(Theme::accent);
                g.fillRect(x, y0 + h - 6.0 - (h - 12.0) * v, bar - gap, (h - 12.0) * v);
            }
        }
    }

    void factoryPatches(Patches& p) {
        p.add("Classic").set(bands, 16).set(noise, 25);
        p.add("Robot").set(bands, 8).set(width, 30).set(release, 20).set(noise, 10);
        p.add("Clear Speech")
            .set(bands, 32)
            .set(width, 60)
            .set(attack, 2)
            .set(release, 30)
            .set(noise, 40);
        p.add("Small Throat").set(shift, 5);
        p.add("Choir Pad").set(bands, 24).set(release, 250).set(width, 70).set(noise, 5);
    }
};
