// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// An equaliser, poly: a low shelf, two bell-shaped mids with their own
// widths and a high shelf, each cutting or boosting up to 18 dB, with a
// low cut and a high cut (12 dB an octave) to clear rumble and fizz.
// At its defaults it changes nothing: every band flat, both cuts open
// (Low Cut at its lowest and High Cut at its highest are off). The display
// draws the whole response. Every knob takes a cable, so an envelope can
// sweep a bell for a wah or a filter-like accent.

constexpr int kBands = 6;  // low cut, low shelf, mid 1, mid 2, high shelf, high cut

struct Eq : Module {
    static constexpr ModuleInfo info = {
        .name = "EQ",
        .id = "com.suppleaudio.rack.eq",
        .vendor = "Supple Audio",
        .description =
            "An equaliser: low and high shelves, two bells with width, low and high cuts; "
            "flat at its defaults.",
        .tags = {"filter", "eq", "poly"}};

    Param lowFreq = frequency("Low Freq", 30, 500, 100).in("Low").help("The low shelf's corner.");
    Param lowGain =
        decibels("Low Gain", -18, 18, 0).in("Low").help("The low shelf's cut or boost.");
    Param mid1Freq =
        frequency("Mid 1 Freq", 60, 4000, 400).in("Mid 1").help("The first bell's centre.");
    Param mid1Gain =
        decibels("Mid 1 Gain", -18, 18, 0).in("Mid 1").help("The first bell's cut or boost.");
    Param mid1Q = ratio("Mid 1 Q", 0.3, 8, 1)
                      .in("Mid 1")
                      .help("The first bell's narrowness: 1 spans about an octave and a third.");
    Param mid2Freq =
        frequency("Mid 2 Freq", 300, 12000, 2500).in("Mid 2").help("The second bell's centre.");
    Param mid2Gain =
        decibels("Mid 2 Gain", -18, 18, 0).in("Mid 2").help("The second bell's cut or boost.");
    Param mid2Q = ratio("Mid 2 Q", 0.3, 8, 1).in("Mid 2").help("The second bell's narrowness.");
    Param highFreq =
        frequency("High Freq", 1500, 18000, 8000).in("High").help("The high shelf's corner.");
    Param highGain =
        decibels("High Gain", -18, 18, 0).in("High").help("The high shelf's cut or boost.");
    Param lowCut =
        frequency("Low Cut", 20, 1000, 20).in("Cuts").help("A 12 dB high pass: off at 20 Hz.");
    Param highCut = frequency("High Cut", 1000, 20000, 20000)
                        .in("Cuts")
                        .help("A 12 dB low pass: off at 20 kHz.");
    Param level = decibels("Level", -18, 18, 0).smoothed(20).in("Output").help("The gain after.");

    Input in = audioIn("In").help("What to equalise. Poly: each channel alike.");
    Output out = audioOut("Out").help("The equalised sound.");

    Array<Biquad, 16 * kBands> filter;
    Array<Biquad, kBands> shown;  // the editor's, for drawing

    bool cutsLow() const { return lowCut.value() > 20.5; }
    bool cutsHigh() const { return highCut.value() < 19900.0; }

    // Band b of channel c (or of the drawing) set to the knobs.
    void design(Biquad& f, int b) const {
        const double top = 0.45 * sampleRate();
        if (b == 0) f.highpass(lowCut.value(), 0.7071);
        if (b == 1) f.lowShelf(lowFreq.value(), 0.7071, lowGain.value());
        if (b == 2) f.peak(min(mid1Freq.value(), top), mid1Q.value(), mid1Gain.value());
        if (b == 3) f.peak(min(mid2Freq.value(), top), mid2Q.value(), mid2Gain.value());
        if (b == 4) f.highShelf(min(highFreq.value(), top), 0.7071, highGain.value());
        if (b == 5) f.lowpass(min(highCut.value(), top), 0.7071);
    }

    void process(int frames) {
        const bool low = cutsLow(), high = cutsHigh();
        for (int c = 0; c < channels(); ++c) {
            for (int b = 0; b < kBands; ++b) design(filter[c * kBands + b], b);
            for (int i = 0; i < frames; ++i) {
                double x = in[c][i];
                if (low) x = filter[c * kBands].process(x);
                for (int b = 1; b < 5; ++b) x = filter[c * kBands + b].process(x);
                if (high) x = filter[c * kBands + 5].process(x);
                out[c][i] = x * pow(10.0, level.at(i, c) / 20.0);
            }
        }
    }

    void panel(Panel& p) {
        p.size(200, 330);
        p.display(8, 28, 184, 84);
        p.knob(lowGain, 28, 148);
        p.knob(mid1Gain, 76, 148);
        p.knob(mid2Gain, 124, 148);
        p.knob(highGain, 172, 148);
        p.knob(lowFreq, 28, 202, 28);
        p.knob(mid1Freq, 76, 202, 28);
        p.knob(mid2Freq, 124, 202, 28);
        p.knob(highFreq, 172, 202, 28);
        p.knob(lowCut, 28, 252, 28);
        p.knob(mid1Q, 76, 252, 28);
        p.knob(mid2Q, 124, 252, 28);
        p.knob(highCut, 172, 252, 28);
        p.input(in, 28, 304);
        p.knob(level, 100, 302, 28);
        p.output(out, 172, 304);
    }

    void draw(Canvas& g) {
        const double x0 = 8.0, y0 = 28.0, w = 184.0, h = 84.0;
        g.well(x0, y0, w, h);
        const double lowHz = 20.0, highHz = 20000.0, range = 20.0;  // dB either way
        g.strokeStyle(Theme::line);
        g.lineWidth(1.0);
        g.beginPath();
        g.moveTo(x0 + 4.0, dbToY(0.0, -range, range, y0, h));
        g.lineTo(x0 + w - 4.0, dbToY(0.0, -range, range, y0, h));
        g.stroke();
        for (int b = 0; b < kBands; ++b) design(shown[b], b);
        const bool low = cutsLow(), high = cutsHigh();
        g.strokeStyle(Theme::accent);
        g.lineWidth(1.6);
        g.lineJoin(LineJoin::Round);
        g.beginPath();
        for (int j = 0; j <= 120; ++j) {
            const double hz = lowHz * pow(highHz / lowHz, j / 120.0);
            double db = level.value();
            if (low) db += shown[0].responseDb(hz);
            for (int b = 1; b < 5; ++b) db += shown[b].responseDb(hz);
            if (high) db += shown[5].responseDb(hz);
            const double x = hzToX(hz, lowHz, highHz, x0 + 4.0, w - 8.0);
            const double y = dbToY(clamp(db, -range, range), -range, range, y0 + 4.0, h - 8.0);
            if (j == 0)
                g.moveTo(x, y);
            else
                g.lineTo(x, y);
        }
        g.stroke();
    }

    void factoryPatches(Patches& p) {
        p.add("Warm").set(lowGain, 3).set(lowFreq, 150).set(highGain, -3).set(highFreq, 9000);
        p.add("Presence").set(mid2Gain, 4).set(mid2Freq, 3500).set(mid2Q, 0.8).set(lowCut, 80);
        p.add("Scoop")
            .set(mid1Gain, -6)
            .set(mid1Freq, 600)
            .set(mid1Q, 0.7)
            .set(lowGain, 3)
            .set(highGain, 3);
        p.add("Telephone").set(lowCut, 400).set(highCut, 3200).set(mid2Gain, 6).set(mid2Freq, 1500);
        p.add("Air").set(highGain, 5).set(highFreq, 12000).set(lowCut, 40);
    }
};
