// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A wavetable oscillator, poly: Table picks a family of single-cycle waves
// and Position sweeps smoothly through it, cross-fading from one wave to
// the next. Patch an envelope or an LFO into Position: that sweep is the
// sound. The tables:
//
//   Classic  sine, triangle, saw, square, narrowing pulses
//   Sync     a saw synced to a faster one, its ratio climbing
//   Fold     a sine folded over itself harder and harder
//   Formant  a windowed sine whose overtone climbs: a resonant sweep
//   Vowel    a buzz through the formants of a, e, i, o, u
//   Organ    harmonics added one by one, as drawbars pulled out
//   Digital  a sine on fewer and fewer levels: from smooth to 8-bit steps
//
// Unison stacks up to seven copies spread over Detune, for thick leads and
// pads. The display shows the wave being played.
//
// The waves are computed when the module starts, from formulas, not
// stored: each is turned into its harmonics, and kept as nine versions
// with fewer and fewer of them, one per octave of pitch, so no harmonic
// ever passes half the sample rate: it does not alias at any pitch. The
// highest harmonic played lies between a quarter and a half of the sample
// rate (12 to 24 kHz at 48 kHz), depending on the note. Vowel's
// formants are those of a voice at 110 Hz, and move with the pitch as any
// wavetable's do. Every wave peaks at 1.

constexpr double kC4 = 261.6255653005986;
constexpr int kTables = 7;
constexpr int kMaxFrames = 8;
constexpr int kMips = 9;
constexpr int kSize = 2048;      // the samples a wave is computed at
constexpr int kHarmonics = 256;  // the most a wave keeps (its lowest version)
constexpr int kShown = 96;
// How far up an octave (log2 of 4/3) the richer version keeps playing.
constexpr double kReach = 0.4150375;

int framesOf(int table) { return table == 4 ? 5 : 8; }
// The harmonics version m keeps, and its length in samples (16 per cycle
// of its top harmonic, at least 64).
int harmonicsOf(int m) {
    return m == 0
               ? 256
               : (m == 1
                      ? 128
                      : (m == 2
                             ? 64
                             : (m == 3
                                    ? 32
                                    : (m == 4 ? 16
                                              : (m == 5 ? 8 : (m == 6 ? 4 : (m == 7 ? 2 : 1)))))));
}
int lengthOf(int m) { return max(16 * harmonicsOf(m), 64); }

// The classic shapes, each rising through 0 at the start as the sine does.
double triOf(double q) { return q < 0.25 ? 4.0 * q : (q < 0.75 ? 2.0 - 4.0 * q : 4.0 * q - 4.0); }
double sawOf(double q) { return q < 0.5 ? 2.0 * q : 2.0 * q - 2.0; }
double squareOf(double q) { return q < 0.5 ? 1.0 : -1.0; }
double pulseOf(double q, double w) { return q < w ? 1.0 : -1.0; }

// Formant f (0..3) of vowel v (0..4) in Hz, and its width; the vowel
// module's.
double formantOf(int v, int f) {
    if (f == 3) return 3500.0;
    if (v == 0) return f == 0 ? 750.0 : (f == 1 ? 1200.0 : 2600.0);
    if (v == 1) return f == 0 ? 480.0 : (f == 1 ? 1850.0 : 2550.0);
    if (v == 2) return f == 0 ? 300.0 : (f == 1 ? 2250.0 : 3000.0);
    if (v == 3) return f == 0 ? 470.0 : (f == 1 ? 850.0 : 2500.0);
    return f == 0 ? 330.0 : (f == 1 ? 750.0 : 2400.0);
}
double widthOf(int f) { return f == 0 ? 90.0 : (f == 1 ? 110.0 : (f == 2 ? 150.0 : 200.0)); }
double levelOf(int f) { return f == 0 ? 1.0 : (f == 1 ? 0.7 : (f == 2 ? 0.35 : 0.2)); }

// Wave `frame` of a time-drawn table at phase q (0..1).
double drawn(int table, int frame, double q) {
    const double s = sin(twoPi * q);
    if (table == 0) {
        if (frame == 0) return s;
        if (frame == 1) return triOf(q);
        if (frame == 2) return 0.5 * (triOf(q) + sawOf(q));
        if (frame == 3) return sawOf(q);
        if (frame == 4) return 0.5 * (sawOf(q) + squareOf(q));
        if (frame == 5) return squareOf(q);
        return pulseOf(q, frame == 6 ? 0.25 : 0.1);
    }
    if (table == 1) {
        // The synced saw's speed against the cycle's.
        const double r = frame == 0   ? 1.0
                         : frame == 1 ? 1.5
                         : frame == 2 ? 2.0
                         : frame == 3 ? 2.75
                         : frame == 4 ? 3.5
                         : frame == 5 ? 4.5
                         : frame == 6 ? 5.75
                                      : 7.0;
        const double u = r * q + 0.5;
        return 2.0 * (u - floor(u)) - 1.0;
    }
    if (table == 2) return sin(0.5 * pi * (1.0 + 1.15 * frame) * s);
    if (table == 3) {
        const double k = frame == 0   ? 1.0
                         : frame == 1 ? 2.0
                         : frame == 2 ? 3.0
                         : frame == 3 ? 4.5
                         : frame == 4 ? 6.0
                         : frame == 5 ? 8.0
                         : frame == 6 ? 11.0
                                      : 15.0;
        return sin(twoPi * k * q) * (0.5 - 0.5 * cos(twoPi * q));
    }
    // Digital: the sine on 2n + 1 levels.
    const double n = frame == 0   ? 24.0
                     : frame == 1 ? 12.0
                     : frame == 2 ? 7.0
                     : frame == 3 ? 5.0
                     : frame == 4 ? 4.0
                     : frame == 5 ? 3.0
                     : frame == 6 ? 2.0
                                  : 1.0;
    return roundToInt(s * n) / n;
}

// Harmonic k's amplitude in a spectrum-drawn table's wave.
double harmonicOf(int table, int frame, int k) {
    if (table == 4) {
        const double hz = 110.0 * k;
        double a = 0.0;
        for (int f = 0; f < 4; ++f) {
            const double d = (hz - formantOf(frame, f)) / widthOf(f);
            a += levelOf(f) / (1.0 + d * d);
        }
        return a / sqrt(k);
    }
    // Organ: frame n has the first n + 1 of these harmonics.
    const int order = k == 1    ? 0
                      : k == 2  ? 1
                      : k == 3  ? 2
                      : k == 4  ? 3
                      : k == 6  ? 4
                      : k == 8  ? 5
                      : k == 5  ? 6
                      : k == 10 ? 6
                      : k == 12 ? 7
                      : k == 16 ? 7
                                : 99;
    if (order > frame) return 0.0;
    return 1.0 / sqrt(k);
}

// Wave `frame` of `table` at phase q, from its formula and without the
// band limit, for the display before any sound has played.
double sketchOf(int table, int frame, double q) {
    if (table != 4 && table != 5) return drawn(table, frame, q);
    double y = 0.0;
    for (int k = 1; k <= 48; ++k)
        y += harmonicOf(table, frame, k) * cos(twoPi * k * q + pi * k * k / 16.0 - 0.5 * pi);
    return y;
}

struct Voice {
    Array<double, 7> phase;
};

struct Wavetable : Module {
    static constexpr ModuleInfo info = {
        .name = "Wavetable",
        .id = "com.suppleaudio.rack.wavetable",
        .vendor = "Supple Audio",
        .description =
            "A wavetable oscillator: seven tables of its own, a position to sweep, "
            "unison.",
        .tags = {"oscillator", "wavetable", "poly"}};

    Param table =
        choice("Table", {"Classic", "Sync", "Fold", "Formant", "Vowel", "Organ", "Digital"}, 0)
            .help("The family of waves Position sweeps through.");
    Param position =
        percent("Position", 40)
            .help("Where in the table: 0 its first wave, 100 its last, cross-fading between.");
    Param coarse = semitones("Coarse", -48, 48, 0).help("Transposes the pitch in semitones.");
    Param fine = cents("Fine", -100, 100, 0).help("Detunes the pitch in cents.");
    Param fmDepth =
        octaves("FM Depth", -4, 4, 1).help("How far 1 at FM moves the pitch, in octaves.");
    Param unison = integer("Unison", 1, 7, 1).help("How many detuned copies play at once.");
    Param detune = cents("Detune", 0, 50, 15)
                       .help("How far the outermost unison copies are detuned, either way.");

    Input pitch = pitchIn("Pitch").help("1 per octave; 0 is C4. Poly: one voice per channel.");
    Input fm = cvIn("FM").help("Moves the pitch exponentially: 1 is FM Depth's octaves.");

    Output out = audioOut("Out").help("The wave, peaking at 1 (more with Unison).");

    Buffer<double> wave;                    // every table's waves, each as kMips versions
    Buffer<double> re, im, specRe, specIm;  // the FFT's work, and a wave's harmonics
    Array<int, kMips> mipStart;
    Array<Voice, 16> voice;
    SharedArray<float, kShown> shown;
    Shared<double> shownPosition;
    Shared<int> shownFrames;

    // Where wave w's version m starts in `wave`: its samples, wrapped round
    // by one before and two after.
    int stride() const { return mipStart[kMips - 1] + lengthOf(kMips - 1) + 3; }
    int startOf(int tb, int frame, int m) const {
        return (tb * kMaxFrames + frame) * stride() + mipStart[m];
    }

    // An in-place complex FFT of re/im's first n (a power of 2): sign -1
    // forward, +1 inverse (unscaled).
    void fft(int n, double sign) {
        int j = 0;
        for (int i = 0; i < n - 1; ++i) {
            if (i < j) {
                const double tr = re[i], ti = im[i];
                re[i] = re[j];
                im[i] = im[j];
                re[j] = tr;
                im[j] = ti;
            }
            int m = n / 2;
            while (m >= 1 && j >= m) {
                j -= m;
                m = m / 2;
            }
            j += m;
        }
        for (int len = 2; len <= n; len *= 2) {
            const int half = len / 2;
            const double step = sign * twoPi / len;
            for (int k = 0; k < half; ++k) {
                const double wr = cos(step * k), wi = sin(step * k);
                for (int s = 0; s < n; s += len) {
                    const int a = s + k, b = s + k + half;
                    const double tr = wr * re[b] - wi * im[b];
                    const double ti = wr * im[b] + wi * re[b];
                    re[b] = re[a] - tr;
                    im[b] = im[a] - ti;
                    re[a] = re[a] + tr;
                    im[a] = im[a] + ti;
                }
            }
        }
    }

    // Computes one wave: its harmonics into spec, then each version.
    void build(int tb, int frame) {
        if (tb == 4 || tb == 5) {
            // Drawn as harmonics: sines, their phases spread so the peaks
            // stay low.
            for (int k = 1; k <= kHarmonics; ++k) {
                const double a = harmonicOf(tb, frame, k);
                const double ph = pi * k * k / 16.0 - 0.5 * pi;
                specRe[k] = a * cos(ph);
                specIm[k] = a * sin(ph);
            }
        } else {
            for (int i = 0; i < kSize; ++i) {
                re[i] = drawn(tb, frame, static_cast<double>(i) / kSize);
                im[i] = 0.0;
            }
            fft(kSize, -1.0);
            for (int k = 1; k <= kHarmonics; ++k) {
                specRe[k] = 2.0 * re[k] / kSize;
                specIm[k] = 2.0 * im[k] / kSize;
            }
        }
        double gain = 1.0;
        for (int m = 0; m < kMips; ++m) {
            const int n = lengthOf(m);
            for (int i = 0; i < n; ++i) {
                re[i] = 0.0;
                im[i] = 0.0;
            }
            for (int k = 1; k <= harmonicsOf(m); ++k) {
                re[k] = specRe[k];
                im[k] = specIm[k];
            }
            fft(n, 1.0);
            if (m == 0) {
                double top = 1e-9;
                for (int i = 0; i < n; ++i) top = max(top, abs(re[i]));
                gain = 1.0 / top;
            }
            const int at = startOf(tb, frame, m);
            for (int i = -1; i < n + 2; ++i) {
                const int from = i < 0 ? i + n : (i >= n ? i - n : i);
                wave[at + 1 + i] = re[from] * gain;
            }
        }
    }

    void prepare() {
        int at = 0;
        for (int m = 0; m < kMips; ++m) {
            mipStart[m] = at;
            at += lengthOf(m) + 3;
        }
        wave.resize(kTables * kMaxFrames * stride());
        re.resize(lengthOf(0));
        im.resize(lengthOf(0));
        specRe.resize(kHarmonics + 1);
        specIm.resize(kHarmonics + 1);
        for (int t = 0; t < kTables; ++t)
            for (int f = 0; f < framesOf(t); ++f) build(t, f);
        for (int c = 0; c < 16; ++c)
            for (int v = 0; v < 7; ++v) {
                // Unison copies start apart, so they do not sum to a spike.
                const double u = 0.618034 * v;
                voice[c].phase[v] = u - floor(u);
            }
    }

    // Version m of a wave at phase p, through a cubic.
    double read(int start, int m, double p) const {
        const int n = lengthOf(m);
        const double x = p * n;
        const int i = min(floorToInt(x), n - 1);
        const double t = x - i;
        const int at = start + mipStart[m] + i;
        const double y0 = wave[at], y1 = wave[at + 1], y2 = wave[at + 2], y3 = wave[at + 3];
        const double c1 = 0.5 * (y2 - y0);
        const double c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
        const double c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + y1;
    }

    void process(int frames) {
        const double sr = sampleRate();
        const int base = stride();
        for (int c = 0; c < channels(); ++c) {
            Voice& vc = voice[c];
            for (int i = 0; i < frames; ++i) {
                const int t = clamp(roundToInt(table.at(i, c)), 0, kTables - 1);
                const double x = clamp(position.at(i, c), 0.0, 1.0) * (framesOf(t) - 1);
                const int f0 = min(floorToInt(x), framesOf(t) - 2);
                const double blend = x - f0;
                const int a = (t * kMaxFrames + f0) * base;
                const int b = a + base;
                const double oct = pitch[c][i] + coarse.at(i, c) / 12.0 + fine.at(i, c) / 1200.0 +
                                   fmDepth.at(i, c) * fm[c][i];
                const int n = clamp(roundToInt(unison.at(i, c)), 1, 7);
                const double spread = detune.at(i, c) / 1200.0;
                double y = 0.0;
                for (int v = 0; v < n; ++v) {
                    const double offset = n == 1 ? 0.0 : spread * (2.0 * v / (n - 1) - 1.0);
                    const double hz = kC4 * exp2(clamp(oct + offset, -8.0, 8.0));
                    const double inc = hz / sr;
                    // The version whose top harmonic is at most 3/4 of the
                    // way to half the sample rate, blended with the next
                    // richer one until that one's top harmonic reaches it.
                    const double level = clamp(log2(inc * 256.0 / 0.375), 0.0, kMips - 1.0);
                    const int m = min(floorToInt(level), kMips - 2);
                    const double rich = clamp(1.0 - (level - m) / kReach, 0.0, 1.0);
                    const double p = vc.phase[v];
                    double s = lerp(read(a, m + 1, p), read(b, m + 1, p), blend);
                    if (rich > 0.0) s = lerp(s, lerp(read(a, m, p), read(b, m, p), blend), rich);
                    // Above the highest version's reach, it fades out.
                    const double fade = clamp((0.5 - inc) * 20.0, 0.0, 1.0);
                    y += fade * s;
                    const double next = p + inc;
                    vc.phase[v] = next - floor(next);
                }
                out[c][i] = y / sqrt(n);
            }
        }
        // The wave channel 1 plays, for the display.
        const int t = clamp(roundToInt(table.at(frames - 1, 0)), 0, kTables - 1);
        const double x = clamp(position.at(frames - 1, 0), 0.0, 1.0) * (framesOf(t) - 1);
        const int f0 = min(floorToInt(x), framesOf(t) - 2);
        const int a = (t * kMaxFrames + f0) * base;
        for (int j = 0; j < kShown; ++j) {
            const double p = static_cast<double>(j) / kShown;
            shown[j] = static_cast<float>(lerp(read(a, 2, p), read(a + base, 2, p), x - f0));
        }
        shownPosition.set(x);
        shownFrames.set(framesOf(t));
    }

    void panel(Panel& p) {
        p.size(176, 292);
        p.display(8, 28, 160, 60);
        p.choice(table, 8, 94, 160, 18);
        p.knob(position, 44, 148, 52);
        p.knob(unison, 110, 140);
        p.knob(detune, 154, 140);
        p.knob(coarse, 22, 206);
        p.knob(fine, 66, 206);
        p.knob(fmDepth, 110, 206);
        p.input(pitch, 22, 262);
        p.input(fm, 66, 262);
        p.output(out, 154, 262);
    }

    void draw(Canvas& g) {
        const double x0 = 8.0, y0 = 28.0, w = 160.0, h = 60.0;
        g.well(x0, y0, w, h);
        const double mid = y0 + 26.0;
        const double half = 20.0;
        g.strokeStyle(Theme::line);
        g.lineWidth(1.0);
        g.beginPath();
        g.moveTo(x0 + 4, mid);
        g.lineTo(x0 + w - 4, mid);
        g.stroke();
        // The wave, one cycle: as played, or until something plays, as the
        // knobs set it.
        Array<double, kShown + 1> y;
        int n = shownFrames.get();
        double at = shownPosition.get();
        if (n > 0) {
            for (int j = 0; j < kShown; ++j) y[j] = static_cast<double>(shown[j]);
        } else {
            const int t = clamp(table.index(), 0, kTables - 1);
            n = framesOf(t);
            at = clamp(position.value(), 0.0, 1.0) * (n - 1);
            const int f0 = min(floorToInt(at), n - 2);
            double top = 1e-9;
            for (int j = 0; j < kShown; ++j) {
                const double q = static_cast<double>(j) / kShown;
                y[j] = lerp(sketchOf(t, f0, q), sketchOf(t, f0 + 1, q), at - f0);
                top = max(top, abs(y[j]));
            }
            for (int j = 0; j < kShown; ++j) y[j] = y[j] / top;
        }
        y[kShown] = y[0];
        g.strokeStyle(Theme::accent);
        g.lineWidth(1.6);
        g.lineJoin(LineJoin::Round);
        g.beginPath();
        for (int j = 0; j <= kShown; ++j) {
            const double v = clamp(y[j], -1.2, 1.2);
            const double x = x0 + 6.0 + (w - 12.0) * j / kShown;
            if (j == 0)
                g.moveTo(x, mid - half * v);
            else
                g.lineTo(x, mid - half * v);
        }
        g.stroke();
        // The table's waves as ticks, and where Position is among them.
        n = clamp(n, 2, kMaxFrames);
        const double ty = y0 + h - 7.0;
        g.fillStyle(Theme::textFaint);
        for (int k = 0; k < n; ++k)
            g.fillRect(x0 + 10.0 + (w - 20.0) * k / (n - 1) - 0.5, ty - 2.0, 1.0, 4.0);
        g.fillStyle(Theme::text);
        g.beginPath();
        g.circle(x0 + 10.0 + (w - 20.0) * clamp(at / (n - 1), 0.0, 1.0), ty, 2.6);
        g.fill();
    }

    void factoryPatches(Patches& p) {
        p.add("Supersaw")
            .set(table, "classic")
            .set(position, 42.857)
            .set(unison, 7)
            .set(detune, 18);
        p.add("Sync Sweep").set(table, "sync").set(position, 50);
        p.add("Vowel Choir").set(table, "vowel").set(position, 0).set(unison, 3).set(detune, 8);
        p.add("Drawbars").set(table, "organ").set(position, 70);
        p.add("Chip").set(table, "digital").set(position, 85);
    }
};
