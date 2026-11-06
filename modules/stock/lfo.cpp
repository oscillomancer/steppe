// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A low-frequency oscillator: seven shapes, a free rate in Hz or a note
// division locked to the song, a phase offset, a width that skews the
// triangle and sine and sets the square's duty, and poly spread. At Voices
// above 1 it sends that many channels, each a share of a cycle apart: four
// voices at 100 % spread are four LFOs in quadrature, one cable to four
// filters. Synced, its phase comes from the song position, so a sweep lands
// on the same beat on every play and in every render; the random shapes
// draw their values from the cycle's number too, so they repeat with the
// song. A cable into Shape morphs between neighbouring shapes; a phase into
// Phase In (a bar's phase from Transport) drives it instead of its own rate.

constexpr int kShapes = 7;

// A random value for cycle n of a channel, -1..1: the same wherever the cycle
// plays.
double cycleValue(int n, int salt) {
    int h = n * 668265261 + salt * 374761393 + 1013904223;
    h = h ^ ((h >> 15) & 131071);
    h = h * 739982445;
    h = h ^ ((h >> 12) & 1048575);
    h = h * 695872825;
    h = h ^ ((h >> 15) & 131071);
    return static_cast<double>((h >> 8) & 65535) / 32767.5 - 1.0;
}

// Quarter notes in a cycle for each division.
double divisionBeats(int k) {
    switch (k) {
        case 0:
            return 64.0;
        case 1:
            return 32.0;
        case 2:
            return 16.0;
        case 3:
            return 8.0;
        case 4:
            return 4.0;
        case 5:
            return 2.0;
        case 6:
            return 1.0;
        case 7:
            return 0.5;
        case 8:
            return 0.25;
        case 9:
            return 0.125;
        case 10:
            return 3.0;
        case 11:
            return 1.5;
        case 12:
            return 0.75;
        case 13:
            return 0.375;
        case 14:
            return 4.0 / 3.0;
        case 15:
            return 2.0 / 3.0;
        case 16:
            return 1.0 / 3.0;
        default:
            return 1.0 / 6.0;
    }
}

// Shape k at phase p (0..1) of cycle n, -1..1. Width w skews the sine and
// triangle (their peak at w) and is the square's duty.
double shapeAt(int k, double p, double w, int n, int salt) {
    if (k == 0) {
        const double q = p < w ? 0.5 * p / w : 0.5 + 0.5 * (p - w) / (1.0 - w);
        return -cos(twoPi * q);
    }
    if (k == 1) return p < w ? -1.0 + 2.0 * p / w : 1.0 - 2.0 * (p - w) / (1.0 - w);
    if (k == 2) return 1.0 - 2.0 * p;
    if (k == 3) return -1.0 + 2.0 * p;
    if (k == 4) return p < w ? 1.0 : -1.0;
    if (k == 5) return cycleValue(n, salt);
    const double a = cycleValue(n, salt);
    const double b = cycleValue(n + 1, salt);
    return a + (b - a) * (0.5 - 0.5 * cos(pi * p));
}

struct Lfo : Module {
    static constexpr ModuleInfo info = {
        .name = "LFO",
        .id = "com.suppleaudio.rack.lfo",
        .vendor = "Supple Audio",
        .description = "A low-frequency oscillator, free or synced to the song, with poly spread.",
        .tags = {"lfo", "modulation", "poly"}};

    Param shape =
        choice("Shape",
               {"Sine", "Triangle", "Saw", "Ramp", "Square", "Stepped Random", "Smooth Random"}, 0)
            .in("Wave")
            .help(
                "The wave: saw falls, ramp rises; stepped random holds a new value "
                "each cycle, smooth random glides between them. A cable morphs "
                "between neighbours.");
    Param rate =
        frequency("Rate", 0.01, 100, 1).in("Time").help("Cycles a second while not synced.");
    Param sync =
        toggle("Sync", false)
            .in("Time")
            .help("Lock the cycle to the song's tempo and position, one cycle a division.");
    Param division =
        choice("Division",
               {"16/1", "8/1", "4/1", "2/1", "1/1", "1/2", "1/4", "1/8", "1/16", "1/32",
                "1/2 dotted", "1/4 dotted", "1/8 dotted", "1/16 dotted", "1/2 triplet",
                "1/4 triplet", "1/8 triplet", "1/16 triplet"},
               6)
            .keys({"d16_1", "d8_1", "d4_1", "d2_1", "d1_1", "d1_2", "d1_4", "d1_8", "d1_16",
                   "d1_32", "d1_2_dotted", "d1_4_dotted", "d1_8_dotted", "d1_16_dotted",
                   "d1_2_triplet", "d1_4_triplet", "d1_8_triplet", "d1_16_triplet"})
            .activeWhen(sync)
            .in("Time")
            .help(
                "The length of a cycle while synced, in notes: 1/1 is four beats, "
                "16/1 sixteen whole notes.");
    Param offset = degrees("Offset", 0, 360, 0)
                       .in("Wave")
                       .help("Where in its cycle it is, 0 to 360 degrees: 180 starts at the top.");
    Param width =
        percent("Width", 50, 1, 99)
            .in("Wave")
            .help(
                "The square's time high, and where the sine and triangle peak: low is a quick "
                "rise and long fall.");
    Param voices =
        integer("Voices", 1, 16, 1)
            .in("Poly")
            .help("How many channels it sends, each a share of a cycle apart by the spread.");
    Param spread =
        percent("Spread", 100)
            .in("Poly")
            .help(
                "How far apart the channels' phases are: 100 % shares one cycle among them evenly "
                "(four voices in quadrature), 0 % keeps them together.");
    Param polarity = choice("Polarity", {"Bipolar", "Unipolar"}, 0)
                         .in("Output")
                         .help("Bipolar swings -1 to 1; unipolar 0 to 1, for levels and mixes.");
    Param level = percent("Level", 100)
                      .smoothed(10)
                      .in("Output")
                      .help("How far it swings: 100 % is the full range.");

    Input reset = gateIn("Reset").help(
        "A rising edge starts the cycle over: free, from the offset; synced, from this beat.");
    Input drive =
        phaseIn("Phase In")
            .withKey("phase_in")
            .help("A 0..1 ramp that drives it instead of its rate (a bar's phase from Transport).");

    Output out = cvOut("Out").help("The wave, one channel a voice.");
    Output phase = phaseOut("Phase").help("Each channel's place in its cycle, 0 to 1.");
    Output trigger = gateOut("Trigger").help("A 1 ms trigger as each channel's cycle begins.");

    struct Voice {
        double cycles = 0.0;  // free running: whole cycles and the phase
        int index = 0;        // the cycle playing
        bool started = false;
        double lastIn = 0.0;  // Phase In's last value, to count its wraps
        int wraps = 0;
        int pulse = 0;
    };
    Array<Voice, 16> state;
    bool resetHigh = false;
    double anchor = 0.0;     // the beat a synced reset started from
    double freeBeats = 0.0;  // the song position, run on at tempo while stopped
    int pulseFrames = 48;
    int salt = 0;

    SharedArray<float, 16> shownPhase;
    Shared<int> shownVoices;

    void factoryPatches(Patches& p) {
        p.add("Slow Sweep").set(shape, "sine").set(rate, 0.1);
        p.add("Bar Swell").set(shape, "triangle").set(sync, true).set(division, "d1_1");
        p.add("Quadrature").set(voices, 4).set(spread, 100).set(rate, 0.5);
        p.add("Random Steps").set(shape, "stepped_random").set(sync, true).set(division, "d1_8");
        p.add("Drift").set(shape, "smooth_random").set(rate, 0.2).set(voices, 8).set(spread, 37);
        p.add("Trance Gate")
            .set(shape, "square")
            .set(sync, true)
            .set(division, "d1_16")
            .set(width, 40)
            .set(polarity, "unipolar");
    }

    void prepare() {
        pulseFrames = max(1, roundToInt(0.001 * sampleRate()));
        salt = seed();
    }

    void process(int frames) {
        const int n = max(channels(), voices.index());
        out.setChannels(n);
        phase.setChannels(n);
        trigger.setChannels(n);
        const Transport t = transport();
        const double beatStep = t.tempo / 60.0 / sampleRate();
        const bool unipolar = polarity.index() == 1;
        const bool driven = drive.connected();
        for (int i = 0; i < frames; ++i) {
            const double beats = t.playing ? t.beats + i * beatStep : freeBeats + beatStep;
            freeBeats = beats;
            const double r = reset[0][i];
            bool restart = false;
            if (!resetHigh && r >= 0.5) {
                resetHigh = true;
                restart = true;
                anchor = beats;
            } else if (resetHigh && r < 0.25) {
                resetHigh = false;
            }
            for (int c = 0; c < n; ++c) {
                Voice& v = state[c];
                const double shift =
                    offset.at(i, c) / 360.0 + clamp(spread.at(i, c), 0.0, 1.0) * c / n;
                double position = 0.0;
                if (driven) {
                    const double x = drive[c][i];
                    if (restart) v.wraps = 0;
                    if (v.started && x < v.lastIn - 0.5) ++v.wraps;
                    if (v.started && x > v.lastIn + 0.5) --v.wraps;
                    v.lastIn = x;
                    position = v.wraps + x + shift;
                } else if (sync.on()) {
                    const double div = divisionBeats(roundToInt(division.at(i, c)));
                    position = (beats - anchor) / div + shift;
                } else {
                    if (restart) v.cycles = 0.0;
                    position = v.cycles + shift;
                    v.cycles += max(0.0, rate.at(i, c)) / sampleRate();
                    if (v.cycles > 1048576.0) v.cycles -= 1048576.0;
                }
                const int index = floorToInt(position);
                const double p = position - index;
                if (restart || (v.started && index != v.index)) v.pulse = pulseFrames;
                v.index = index;
                v.started = true;

                // The shape, morphed between neighbours when a cable moves it.
                const double k = clamp(shape.at(i, c), 0.0, kShapes - 1.0);
                const int k0 = floorToInt(k);
                const int k1 = min(k0 + 1, kShapes - 1);
                const double w = clamp(width.at(i, c), 0.01, 0.99);
                const int s = salt + c * 7919;
                double y = shapeAt(k0, p, w, index, s);
                if (k > k0) y += (shapeAt(k1, p, w, index, s) - y) * (k - k0);
                const double amount = clamp(level.at(i, c), 0.0, 1.0);
                out[c][i] = unipolar ? amount * 0.5 * (y + 1.0) : amount * y;
                phase[c][i] = p;
                trigger[c][i] = v.pulse > 0 ? 1.0 : 0.0;
                if (v.pulse > 0) --v.pulse;
            }
        }
        for (int c = 0; c < 16; ++c)
            shownPhase[c] = c < n ? static_cast<float>(phase[c][frames - 1]) : 0.0f;
        shownVoices.set(n);
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(176, 326);
        p.display(8, 28, 160, 48);
        p.choice(shape, 8, 82, 160, 18);
        p.knob(rate, 22, 130);
        p.knob(offset, 66, 130);
        p.knob(width, 110, 130);
        p.knob(level, 154, 130);
        p.knob(voices, 22, 186);
        p.knob(spread, 66, 186);
        p.toggle(sync, 96, 168, 72, 18);
        p.choice(division, 96, 190, 72, 18);
        p.choice(polarity, 8, 220, 160, 18);
        p.input(reset, 22, 258);
        p.input(drive, 66, 258);
        p.output(out, 66, 298);
        p.output(phase, 110, 298);
        p.output(trigger, 154, 298);
    }

    void draw(Canvas& g) {
        const double x0 = 8.0;
        const double y0 = 28.0;
        const double w = 160.0;
        const double h = 48.0;
        g.well(x0, y0, w, h);
        const double mid = y0 + h * 0.5;
        const double half = h * 0.5 - 5.0;
        g.strokeStyle(Theme::line);
        g.lineWidth(1.0);
        g.beginPath();
        g.moveTo(x0 + 4, mid);
        g.lineTo(x0 + w - 4, mid);
        g.stroke();

        // Two cycles of the shape as it is set.
        const int k = clamp(shape.index(), 0, kShapes - 1);
        const double pw = clamp(width.value(), 0.01, 0.99);
        const int steps = 120;
        g.strokeStyle(Theme::accent);
        g.lineWidth(1.6);
        g.lineJoin(LineJoin::Round);
        g.beginPath();
        for (int j = 0; j <= steps; ++j) {
            const double u = 2.0 * j / steps;
            const int cycle = floorToInt(u);
            const double p = u - cycle;
            const double y = shapeAt(k, p, pw, cycle + 3, 11);
            const double x = x0 + 6.0 + (w - 12.0) * j / steps;
            if (j == 0)
                g.moveTo(x, mid - half * y);
            else
                g.lineTo(x, mid - half * y);
        }
        g.stroke();

        // Each voice's place in the cycle, as a dot on the first cycle.
        const int n = clamp(shownVoices.get(), 1, 16);
        for (int c = 0; c < n; ++c) {
            const double p = clamp(static_cast<double>(shownPhase[c]), 0.0, 1.0);
            const double y = k >= 5 ? 0.0 : shapeAt(k, p, pw, 3, 11);
            g.fillStyle(c == 0 ? Theme::text : Theme::accent2);
            g.beginPath();
            g.circle(x0 + 6.0 + (w - 12.0) * 0.5 * p, mid - half * y, c == 0 ? 3.0 : 2.2);
            g.fill();
        }
    }
};
