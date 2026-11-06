// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// An arpeggiator: the notes held on a poly pitch and gate cable (from Notes,
// or from Chord) played one at a time, a note a clock. Up, down, up and
// down, in the order they were played, at random, or converging from the
// outside in; over one to four octaves. Gate Length sets each note's gate
// as a share of the clock (100 % ties them, legato). Latch keeps the notes
// playing after their keys are up, until a new chord is played. With
// nothing in Clock it plays the song's sixteenths. Reset starts the pattern
// over (and the random order with it).

struct Arp : Module {
    static constexpr ModuleInfo info = {
        .name = "Arp",
        .id = "com.suppleaudio.rack.arp",
        .vendor = "Supple Audio",
        .description = "An arpeggiator over held poly notes: six orders, octaves, latch.",
        .tags = {"arpeggiator", "sequencer", "pitch"}};

    Param mode = choice("Mode", {"Up", "Down", "Up-Down", "As Played", "Random", "Converge"}, 0)
                     .keys({"up", "down", "up_down", "as_played", "random", "converge"})
                     .help(
                         "The order: rising, falling, rising then falling, as the keys went "
                         "down, at random, or lowest and highest closing in.");
    Param octaves = integer("Octaves", 1, 4, 1)
                        .help("How many octaves the pattern climbs before it starts again.");
    Param gateLength =
        percent("Gate Length", 50, 5, 100)
            .help("Each note's gate as a share of the time between clocks; 100 % ties them.");
    Param latch =
        toggle("Latch", false)
            .help("Keep playing the notes after their keys are up, until a new chord starts.");

    Input pitch = pitchIn("Pitch").help("The held notes' pitches, poly (Notes' or Chord's).");
    Input gate = gateIn("Gate").help("The held notes' gates, poly: a note plays while high.");
    Input clock = gateIn("Clock").help(
        "Each rising edge plays the next note; with nothing here, the song's sixteenths.");
    Input reset = gateIn("Reset").help("A rising edge starts the pattern from its first note.");

    Output out = pitchOut("Out").help("The note playing, in octaves from C4.");
    Output outGate = gateOut("Out Gate").help("High for each note's gate length.");

    Array<bool, 16> high;     // each channel's gate, with hysteresis
    Array<bool, 16> latched;  // held, or held by the latch
    Array<int, 16> stamp;     // when each channel's key went down
    Array<int, 16> list;      // the channels playing, in the mode's order
    int stamps = 0;
    int step = 0;
    double note = 0.0;
    bool clockHigh = false;
    bool resetHigh = false;
    int sinceClock = 0;
    int period = 6000;
    int gateLeft = 0;
    bool lowFrame = false;
    bool wasHigh = false;
    int lastSixteenth = -2147483647;
    Random rng;
    int salt = 0;

    void factoryPatches(Patches& p) {
        p.add("Up");
        p.add("Up-Down Two Octaves").set(mode, "up_down").set(octaves, 2);
        p.add("Random Latch").set(mode, "random").set(latch, true).set(octaves, 2);
        p.add("Converge Legato").set(mode, "converge").set(gateLength, 100);
    }

    void prepare() {
        salt = seed();
        rng.seed(salt);
    }

    // The channels on, in the mode's order: by pitch, or by when they went down.
    int gather(int c0, int i, bool byStamp) {
        int m = 0;
        for (int c = 0; c < c0; ++c)
            if (latched[c]) {
                list[m] = c;
                ++m;
            }
        for (int k = 1; k < m; ++k)
            for (int j = k; j > 0; --j) {
                const int a = list[j - 1];
                const int b = list[j];
                const bool later = byStamp ? stamp[b] < stamp[a] : pitch[b][i] < pitch[a][i];
                if (!later) break;
                list[j - 1] = b;
                list[j] = a;
            }
        return m;
    }

    void advance(int n, int i) {
        const int md = clamp(mode.index(), 0, 5);
        const int m = gather(n, i, md == 3);
        if (m == 0) {
            gateLeft = 0;
            return;
        }
        const int total = m * clamp(roundToInt(octaves.at(i)), 1, 4);
        int k = 0;
        if (md == 1) {
            k = total - 1 - wrap(step, total);
        } else if (md == 2) {
            const int cycle = max(1, 2 * total - 2);
            const int q = wrap(step, cycle);
            k = q < total ? q : cycle - q;
        } else if (md == 4) {
            k = rng.below(total);
        } else if (md == 5) {
            const int q = wrap(step, total);
            k = imod(q, 2) == 0 ? q / 2 : total - 1 - q / 2;
        } else {
            k = wrap(step, total);
        }
        ++step;
        note = pitch[list[imod(k, m)]][i] + idiv(k, m);
        const double length = clamp(gateLength.at(i), 0.05, 1.0);
        lowFrame = wasHigh && length < 0.999;
        gateLeft = length >= 0.999 ? -1 : max(1, roundToInt(length * period));
    }

    void process(int frames) {
        out.setChannels(1);
        outGate.setChannels(1);
        const int n = min(16, max(pitch.channels(), gate.channels()));
        const Transport t = transport();
        const bool internal = !clock.connected();
        const double perFrame = t.tempo / 15.0 / sampleRate();
        const bool holding = latch.on();
        for (int i = 0; i < frames; ++i) {
            const double r = reset[0][i];
            if (!resetHigh && r >= 0.5) {
                resetHigh = true;
                step = 0;
                rng.seed(salt);
            } else if (resetHigh && r < 0.25) {
                resetHigh = false;
            }
            // The keys: a new key after all were up starts a new chord.
            bool anyHigh = false;
            for (int c = 0; c < n; ++c) anyHigh = anyHigh || high[c];
            bool startNew = !anyHigh;
            for (int c = 0; c < 16; ++c) {
                const double g = c < n ? gate[c][i] : 0.0;
                if (!high[c] && g >= 0.5) {
                    high[c] = true;
                    if (startNew) {
                        for (int q = 0; q < 16; ++q) latched[q] = false;
                        step = 0;
                        startNew = false;
                    }
                    latched[c] = true;
                    ++stamps;
                    stamp[c] = stamps;
                } else if (high[c] && g < 0.25) {
                    high[c] = false;
                }
                if (!holding && !high[c]) latched[c] = false;
            }

            bool tick = false;
            if (internal) {
                if (t.playing) {
                    const int s = floorToInt(t.beats * 4.0 + i * perFrame + 1e-7);
                    tick = s != lastSixteenth;
                    lastSixteenth = s;
                    period = max(1, roundToInt(sampleRate() * 15.0 / max(1.0, t.tempo)));
                } else {
                    lastSixteenth = -2147483647;
                }
            } else {
                const double c = clock[0][i];
                if (!clockHigh && c >= 0.5) {
                    clockHigh = true;
                    tick = true;
                    if (sinceClock > 0) period = sinceClock;
                    sinceClock = 0;
                } else if (clockHigh && c < 0.25) {
                    clockHigh = false;
                }
            }
            ++sinceClock;
            if (tick) advance(n, i);

            bool on = false;
            if (lowFrame) {
                lowFrame = false;
            } else if (gateLeft != 0) {
                on = true;
                if (gateLeft > 0) --gateLeft;
            }
            out[0][i] = note;
            outGate[0][i] = on ? 1.0 : 0.0;
            wasHigh = on;
        }
    }
};
