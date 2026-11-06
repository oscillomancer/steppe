// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// An envelope follower: audio in, its level out as a CV, poly. Attack and
// Release set how fast the level rises and falls; Detector follows the
// peaks (quick, for drums) or the RMS over about 20 ms (the loudness, for anything
// sustained). Gain scales the envelope. Gate is high while the level is
// above Threshold, and falls only once it drops Hysteresis below it, so a
// wobbling level does not chatter; Onset fires as the gate opens. Duck one
// sound under another with Inverted into a VCA, or trigger an envelope
// from a drum loop.

struct Follower : Module {
    static constexpr ModuleInfo info = {
        .name = "Follower",
        .id = "com.suppleaudio.rack.follower",
        .vendor = "Supple Audio",
        .description = "An envelope follower with a gate and an onset trigger above a threshold.",
        .tags = {"envelope", "analysis", "modulation", "poly"}};

    Param attack = milliseconds("Attack", 0.1, 500, 5)
                       .in("Envelope")
                       .help("How fast the level rises to a louder sound.");
    Param release = milliseconds("Release", 1, 5000, 150)
                        .in("Envelope")
                        .help("How fast the level falls as the sound fades.");
    Param detector = choice("Detector", {"Peak", "RMS"}, 0)
                         .keys({"peak", "rms"})
                         .in("Envelope")
                         .help("Follow the peaks (quick, for drums) or the RMS (the loudness).");
    Param gain = decibels("Gain", -24, 24, 0)
                     .in("Envelope")
                     .help("Scales the envelope out: up for quiet sources.");
    Param threshold = decibels("Threshold", -60, 0, -24)
                          .in("Gate")
                          .help("The level, in dBFS, above which the gate opens.");
    Param hysteresis =
        decibels("Hysteresis", 0, 12, 3)
            .in("Gate")
            .help("How far below the threshold the level must fall for the gate to close.");

    Input in = audioIn("In").help("The sound to follow; poly.");

    Output envelope = unipolarOut("Envelope")
                          .help("The level, 0 for silence and 1 at full scale (times the gain).");
    Output inverted =
        unipolarOut("Inverted").help("1 minus the envelope (from 0 at 1 and above): for ducking.");
    Output gate = gateOut("Gate").help("High while the level is above the threshold.");
    Output onset = gateOut("Onset").help("A 1 ms trigger as the gate opens.");

    struct Voice {
        double level = 0.0;   // the envelope
        double square = 0.0;  // the mean square, for RMS
        bool open = false;
        int pulse = 0;
    };
    Array<Voice, 16> voices;
    int pulseFrames = 48;

    void factoryPatches(Patches& p) {
        p.add("Drums").set(attack, 0.5).set(release, 80).set(threshold, -18);
        p.add("Loudness").set(detector, "rms").set(attack, 30).set(release, 300);
        p.add("Ducking").set(attack, 2).set(release, 250).set(gain, 6);
    }

    void prepare() { pulseFrames = max(1, roundToInt(0.001 * sampleRate())); }

    void process(int frames) {
        const bool rms = detector.index() == 1;
        const double perMs = 0.001 * sampleRate();
        const double meanK = exp(-1.0 / (20.0 * perMs));  // RMS over about 20 ms
        for (int c = 0; c < channels(); ++c) {
            Voice& v = voices[c];
            for (int i = 0; i < frames; ++i) {
                const double x = in[c][i];
                v.square = x * x + (v.square - x * x) * meanK;
                if (!isFinite(v.square)) v.square = 0.0;
                const double target = rms ? sqrt(max(0.0, v.square)) : abs(x);
                const double ms = target > v.level ? attack.at(i, c) : release.at(i, c);
                const double k = exp(-1.0 / max(1.0, ms * perMs));
                v.level = target + (v.level - target) * k;
                if (!isFinite(v.level)) v.level = 0.0;
                const double level = v.level;
                const double db = gainToDb(level);
                if (!v.open && db >= threshold.at(i, c)) {
                    v.open = true;
                    v.pulse = pulseFrames;
                } else if (v.open && db < threshold.at(i, c) - hysteresis.at(i, c)) {
                    v.open = false;
                }
                const double e = level * dbToGain(gain.at(i, c));
                envelope[c][i] = e;
                inverted[c][i] = max(0.0, 1.0 - e);
                gate[c][i] = v.open ? 1.0 : 0.0;
                onset[c][i] = v.pulse > 0 ? 1.0 : 0.0;
                if (v.pulse > 0) --v.pulse;
            }
        }
    }
};
