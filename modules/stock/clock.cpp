// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A clock and a divider-multiplier. Its base is a beat: the song's quarter
// notes (following the song's tempo and position, only while it plays), a
// free-running tempo of its own, or the pulses of a clock patched into
// Clock. Six outputs each run at their own ratio of the base, from a
// sixteenth as fast (/16) to eight times as fast (x8), so one module gives
// sixteenths, bars and triplets at once. Swing delays every second tick of
// the faster outputs; Width sets how long each output's gate stays high.
// Following the song, the divided outputs land on the song's beats: /4 ticks
// on every bar of 4/4. The Reset out fires at play and at each reset, for
// sequencers' resets.

constexpr int kRatios = 16;

// Each ratio as a multiplier (beats per base beat): /16 .. x8.
double ratioOf(int k) {
    switch (k) {
        case 0:
            return 1.0 / 16.0;
        case 1:
            return 1.0 / 12.0;
        case 2:
            return 1.0 / 8.0;
        case 3:
            return 1.0 / 7.0;
        case 4:
            return 1.0 / 6.0;
        case 5:
            return 1.0 / 5.0;
        case 6:
            return 1.0 / 4.0;
        case 7:
            return 1.0 / 3.0;
        case 8:
            return 0.5;
        case 9:
            return 1.0;
        case 10:
            return 2.0;
        case 11:
            return 3.0;
        case 12:
            return 4.0;
        case 13:
            return 5.0;
        case 14:
            return 6.0;
        default:
            return 8.0;
    }
}

// Ticks at or before position p, with every second tick s of a tick late.
double ticksBefore(double p, double s) {
    const double n = floor(p);
    const double f = p - n;
    const bool odd = fmod(abs(n), 2.0) == 1.0;
    return odd && f < s ? n : n + 1.0;
}

struct Clock : Module {
    static constexpr ModuleInfo info = {
        .name = "Clock",
        .id = "com.suppleaudio.rack.clock",
        .vendor = "Supple Audio",
        .description =
            "A clock and divider-multiplier: six ratios of the song, a tempo or a pulse.",
        .tags = {"clock", "sequencer", "utility"}};

    Param followSong = toggle("Follow Song", true)
                           .in("Base")
                           .help(
                               "With nothing in Clock: follow the song's tempo and position, "
                               "ticking only while it "
                               "plays; off, run free at Tempo.");
    Param tempo = continuous("Tempo", 20, 300, 120, Unit::Bpm)
                      .activeUnless(followSong)
                      .in("Base")
                      .help("Beats a minute while running free.");
    Param swing = percent("Swing", 0, 0, 75)
                      .in("Feel")
                      .help(
                          "How late every second tick of the faster-than-a-beat outputs comes, as "
                          "a share of a "
                          "tick: 33 % is a triplet shuffle.");
    Param width = percent("Width", 50, 5, 95)
                      .in("Feel")
                      .help("How long each output's gate stays high, as a share of its tick.");
    Param ratio_1 = choice("Ratio 1",
                           {"/16", "/12", "/8", "/7", "/6", "/5", "/4", "/3", "/2", "x1", "x2",
                            "x3", "x4", "x5", "x6", "x8"},
                           12)
                        .keys({"div16", "div12", "div8", "div7", "div6", "div5", "div4", "div3",
                               "div2", "x1", "x2", "x3", "x4", "x5", "x6", "x8"})
                        .in("Ratios")
                        .help("Output 1's ticks a beat: x4 is sixteenths, /4 a bar of 4/4.");
    Param ratio_2 = choice("Ratio 2",
                           {"/16", "/12", "/8", "/7", "/6", "/5", "/4", "/3", "/2", "x1", "x2",
                            "x3", "x4", "x5", "x6", "x8"},
                           10)
                        .keys({"div16", "div12", "div8", "div7", "div6", "div5", "div4", "div3",
                               "div2", "x1", "x2", "x3", "x4", "x5", "x6", "x8"})
                        .in("Ratios")
                        .help("Output 2's ticks a beat.");
    Param ratio_3 = choice("Ratio 3",
                           {"/16", "/12", "/8", "/7", "/6", "/5", "/4", "/3", "/2", "x1", "x2",
                            "x3", "x4", "x5", "x6", "x8"},
                           9)
                        .keys({"div16", "div12", "div8", "div7", "div6", "div5", "div4", "div3",
                               "div2", "x1", "x2", "x3", "x4", "x5", "x6", "x8"})
                        .in("Ratios")
                        .help("Output 3's ticks a beat.");
    Param ratio_4 = choice("Ratio 4",
                           {"/16", "/12", "/8", "/7", "/6", "/5", "/4", "/3", "/2", "x1", "x2",
                            "x3", "x4", "x5", "x6", "x8"},
                           8)
                        .keys({"div16", "div12", "div8", "div7", "div6", "div5", "div4", "div3",
                               "div2", "x1", "x2", "x3", "x4", "x5", "x6", "x8"})
                        .in("Ratios")
                        .help("Output 4's ticks a beat.");
    Param ratio_5 = choice("Ratio 5",
                           {"/16", "/12", "/8", "/7", "/6", "/5", "/4", "/3", "/2", "x1", "x2",
                            "x3", "x4", "x5", "x6", "x8"},
                           6)
                        .keys({"div16", "div12", "div8", "div7", "div6", "div5", "div4", "div3",
                               "div2", "x1", "x2", "x3", "x4", "x5", "x6", "x8"})
                        .in("Ratios")
                        .help("Output 5's ticks a beat.");
    Param ratio_6 = choice("Ratio 6",
                           {"/16", "/12", "/8", "/7", "/6", "/5", "/4", "/3", "/2", "x1", "x2",
                            "x3", "x4", "x5", "x6", "x8"},
                           11)
                        .keys({"div16", "div12", "div8", "div7", "div6", "div5", "div4", "div3",
                               "div2", "x1", "x2", "x3", "x4", "x5", "x6", "x8"})
                        .in("Ratios")
                        .help("Output 6's ticks a beat.");

    Input clock = gateIn("Clock").help(
        "A pulse a beat to divide and multiply instead of the song or the tempo.");
    Input run =
        gateIn("Run").normal(1.0).help("While low, it stops; running while nothing is connected.");
    Input reset =
        gateIn("Reset").help("A rising edge starts every output over from its first tick.");

    Outputs<6> out = gateOut("Out").help("Ticks at the Ratio beside it.");
    Output resetOut = gateOut("Reset").help(
        "A 1 ms trigger as it starts (at play, following the song) and at each reset.");

    Array<double, 6> lastCount;
    Array<int, 6> gateLeft;
    Array<bool, 6> lowFrame;  // one frame low, so a tick during a gate rises again
    Array<bool, 6> wasHigh;
    double beats = 0.0;   // the base position: beats since the start or the reset
    double anchor = 0.0;  // following the song: the song beat a reset started from
    int pulses = -1;      // an external clock's pulses since the reset
    int sincePulse = 0;
    int pulsePeriod = 0;  // frames between its last two pulses; 0 unknown
    bool running = false;
    Trigger clockEdge;
    Trigger resetEdge;
    bool restart = true;  // the next position starts the counts afresh
    Pulse resetPulse;

    void factoryPatches(Patches& p) {
        p.add("Song Grid");
        p.add("Polyrhythm")
            .set(ratio_1, "x3")
            .set(ratio_2, "x4")
            .set(ratio_3, "x5")
            .set(ratio_4, "div3")
            .set(ratio_5, "div5")
            .set(ratio_6, "div7");
        p.add("Shuffle").set(swing, 33).set(ratio_1, "x4").set(ratio_2, "x2");
        p.add("Free 90").set(followSong, false).set(tempo, 90);
    }

    void prepare() {
        for (int k = 0; k < 6; ++k) {
            lastCount[k] = 0.0;
            gateLeft[k] = 0;
            lowFrame[k] = false;
            wasHigh[k] = false;
        }
    }

    double multiplier(int k, int i) {
        switch (k) {
            case 0:
                return ratioOf(clamp(roundToInt(ratio_1.at(i)), 0, kRatios - 1));
            case 1:
                return ratioOf(clamp(roundToInt(ratio_2.at(i)), 0, kRatios - 1));
            case 2:
                return ratioOf(clamp(roundToInt(ratio_3.at(i)), 0, kRatios - 1));
            case 3:
                return ratioOf(clamp(roundToInt(ratio_4.at(i)), 0, kRatios - 1));
            case 4:
                return ratioOf(clamp(roundToInt(ratio_5.at(i)), 0, kRatios - 1));
            default:
                return ratioOf(clamp(roundToInt(ratio_6.at(i)), 0, kRatios - 1));
        }
    }

    void process(int frames) {
        for (int k = 0; k < 6; ++k) out[k].setChannels(1);
        resetOut.setChannels(1);
        const Transport t = transport();
        const bool external = clock.connected();
        const bool song = !external && followSong.on();
        const double songBeatsPerFrame = t.tempo / 60.0 / sampleRate();
        for (int i = 0; i < frames; ++i) {
            const double songBeats = t.beats + i * songBeatsPerFrame;
            const bool resetNow = resetEdge.rose(reset[0][i]);
            const bool go = run[0][i] >= 0.5 && (!song || t.playing);
            if (go && !running) {
                restart = true;
                resetPulse.fire();
            }
            running = go;
            if (resetNow) {
                beats = 0.0;
                anchor = songBeats;
                pulses = -1;
                restart = true;
                resetPulse.fire();
            }

            // The base position and a beat's length in frames.
            double beatFrames = 60.0 * sampleRate() / clamp(tempo.at(i), 20.0, 300.0);
            if (external) {
                ++sincePulse;
                if (clockEdge.rose(clock[0][i])) {
                    if (pulses >= 0) pulsePeriod = sincePulse;
                    sincePulse = 0;
                    ++pulses;
                }
                const double fraction =
                    pulses >= 0 && pulsePeriod > 0
                        ? min(0.999, static_cast<double>(sincePulse) / pulsePeriod)
                        : 0.0;
                beats = pulses < 0 ? -1.0 : pulses + fraction;
                // Until the period is known, gates as if a pulse came every 100 ms.
                beatFrames = pulsePeriod > 0 ? pulsePeriod : 0.1 * sampleRate();
            } else if (song) {
                if (t.playing) beats = songBeats - anchor;
                beatFrames = 60.0 * sampleRate() / max(1.0, t.tempo);
            }

            const double s = clamp(swing.at(i), 0.0, 0.75);
            const double w = clamp(width.at(i), 0.05, 0.95);
            for (int k = 0; k < 6; ++k) {
                const double m = multiplier(k, i);
                const double shuffle = m > 1.0 ? s : 0.0;
                // A hair ahead, so a tick on a beat the song reaches by
                // summing frames is not a frame late.
                const double p = beats * m + 1e-7;
                if (running && beats >= 0.0) {
                    const double count = ticksBefore(p, shuffle);
                    if (restart) lastCount[k] = ticksBefore(p - 1e-6, shuffle);
                    if (count > lastCount[k]) {
                        // High for its share of the time to the next tick:
                        // longer after an even tick when swung, shorter after
                        // an odd one.
                        const bool even = fmod(abs(count - 1.0), 2.0) == 0.0;
                        const double spacing = even ? 1.0 + shuffle : 1.0 - shuffle;
                        lowFrame[k] = wasHigh[k];
                        gateLeft[k] = max(1, roundToInt(w * spacing * beatFrames / m));
                    }
                    lastCount[k] = count;
                }
                bool high = gateLeft[k] > 0;
                if (lowFrame[k]) {
                    high = false;
                    lowFrame[k] = false;
                }
                out[k][0][i] = high ? 1.0 : 0.0;
                wasHigh[k] = high;
                if (gateLeft[k] > 0) --gateLeft[k];
            }
            if (running && beats >= 0.0) restart = false;
            resetOut[0][i] = resetPulse.next();
            if (running && !external && !song) beats += 1.0 / beatFrames;
        }
    }

    void panel(Panel& p) {
        p.size(176, 412);
        p.knob(tempo, 30, 52);
        p.knob(swing, 88, 52);
        p.knob(width, 146, 52);
        p.toggle(followSong, 8, 86, 160, 18);
        p.input(clock, 30, 128);
        p.input(run, 88, 128);
        p.input(reset, 146, 128);
        p.choice(ratio_1, 8, 160, 116, 18);
        p.output(out[0], 152, 169);
        p.choice(ratio_2, 8, 194, 116, 18);
        p.output(out[1], 152, 203);
        p.choice(ratio_3, 8, 228, 116, 18);
        p.output(out[2], 152, 237);
        p.choice(ratio_4, 8, 262, 116, 18);
        p.output(out[3], 152, 271);
        p.choice(ratio_5, 8, 296, 116, 18);
        p.output(out[4], 152, 305);
        p.choice(ratio_6, 8, 330, 116, 18);
        p.output(out[5], 152, 339);
        p.output(resetOut, 152, 380);
    }
};
