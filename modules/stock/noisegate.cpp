// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A noise gate, poly: lets the sound through while it is louder than
// Thresh, and turns it down by Range while it is quieter. It opens when the
// level reaches Thresh and closes only once it falls Hysteresis below, so a
// note dying around the threshold does not chatter. Once the level is below
// that, it stays open for Hold, then fades down over Release; it opens over
// Attack. Range is how far it turns down: -80 dB is silence, -12 a gentle
// expander that only lowers the noise between notes.
//
// Patch Key and it listens there instead of In: a kick opening a bass or a
// pad, a drum's own close microphone gating its room. Key HP and Key LP
// filter what it hears (both off at their ends), so a kick's low thump
// opens it and the hats bleeding in do not. Linked, every channel opens
// together, by the loudest; unlinked, each alone. Open is a gate, high
// while open (hold included); Gain is the gain it applies, to gate
// something else alike through a VCA.

constexpr int kChannels = 16;

struct Detector {
    Biquad high, low;   // the key filters
    double level = 0;   // the peak follower, linear
    bool open = false;  // above Thresh, then until below Thresh - Hysteresis
    int hold = 0;       // frames left open once below
    double gain = 0;    // linear, smoothed
};

struct NoiseGate : Module {
    static constexpr ModuleInfo info = {
        .name = "Noise Gate",
        .id = "com.suppleaudio.rack.noisegate",
        .vendor = "Supple Audio",
        .description =
            "A noise gate: threshold, hysteresis, attack, hold, release, range; a "
            "filtered key input to open it by.",
        .tags = {"dynamics", "effect", "poly"}};

    Param threshold =
        decibels("Thresh", -80, 0, -50).in("Gate").help("Where it opens: the level must reach it.");
    Param hysteresis =
        decibels("Hysteresis", 0, 24, 6)
            .in("Gate")
            .help("How far below Thresh it closes, so a level hovering there does not chatter.");
    Param range = decibels("Range", -80, 0, -80)
                      .in("Gate")
                      .help("How far down it turns while closed: -80 is silence, -12 an expander.");
    Param attack = milliseconds("Attack", 0.05, 50, 0.5)
                       .in("Timing")
                       .help("How fast it opens: very short can click on a low note.");
    Param holdTime = milliseconds("Hold", 0, 2000, 50)
                         .withKey("hold")
                         .in("Timing")
                         .help("How long it stays open once the level falls below.");
    Param release = milliseconds("Release", 5, 4000, 100)
                        .in("Timing")
                        .help("How slowly it fades down once Hold is over.");
    Param keyHp = frequency("Key HP", 20, 2000, 20)
                      .in("Key")
                      .help("A high pass on what it hears: off at 20 Hz.");
    Param keyLp = frequency("Key LP", 50, 20000, 20000)
                      .in("Key")
                      .help("A low pass on what it hears: off at 20 kHz.");
    Param link =
        toggle("Link", true).in("Key").help("Opens every channel together, by the loudest.");

    Input in = audioIn("In").help("What to gate. Poly: each channel, linked or alone.");
    Input sidechain = audioIn("Key").help(
        "What it listens to instead of In: a kick to open a bass by. A mono cable keys every "
        "channel.");
    Output out = audioOut("Out").help("The gated sound.");
    Output opened = gateOut("Open").withKey("open").help("High while it is open, hold included.");
    Output gain = unipolarOut("Gain").help(
        "The gain it applies, 1 while open: into a VCA, it gates something else alike.");

    Array<Detector, kChannels> detector;

    void prepare() {
        for (int c = 0; c < kChannels; ++c) {
            Detector& d = detector[c];
            d.high.clear();
            d.low.clear();
            d.level = 0.0;
            d.open = false;
            d.hold = 0;
            d.gain = 0.0;
        }
    }

    // One detector's state through a frame at the level heard, in dB.
    void step(Detector& d, double heardDb, double openDb, double closeDb, int holdFrames) {
        if (!d.open) {
            if (heardDb >= openDb) {
                d.open = true;
                d.hold = holdFrames;
            }
        } else if (heardDb >= closeDb) {
            d.hold = holdFrames;
        } else if (d.hold > 0) {
            --d.hold;
        } else {
            d.open = false;
        }
    }

    void process(int frames) {
        const double sr = sampleRate();
        const int n = max(channels(), 1);
        const bool keyed = sidechain.connected();
        const bool linked = link.on();
        const bool highPassed = keyHp.value() > 20.5;
        const bool lowPassed = keyLp.value() < 19999.5;
        opened.setChannels(linked ? 1 : n);
        gain.setChannels(linked ? 1 : n);
        for (int c = 0; c < n; ++c) {
            detector[c].high.highpass(keyHp.value(), 0.7071);
            detector[c].low.lowpass(min(keyLp.value(), 0.45 * sr), 0.7071);
        }
        const double openDb = threshold.value();
        const double closeDb = openDb - max(hysteresis.value(), 0.0);
        const int holdFrames = toInt(max(holdTime.value(), 0.0) * 0.001 * sr);
        // The peak follower falls 1 dB a millisecond: quick enough to close
        // promptly, slow enough to bridge a low note's zero crossings with Hold.
        const double fall = pow(10.0, -1.0 / (20.0 * 0.001 * sr));
        const double up = exp(-1.0 / (max(attack.value(), 0.05) * 0.001 * sr));
        const double down = exp(-1.0 / (max(release.value(), 5.0) * 0.001 * sr));
        const double floorGain = range.value() <= -79.5 ? 0.0 : pow(10.0, range.value() / 20.0);
        for (int i = 0; i < frames; ++i) {
            double loudest = 0.0;
            for (int c = 0; c < n; ++c) {
                Detector& d = detector[c];
                double heard = keyed ? sidechain[c][i] : in[c][i];
                if (highPassed) heard = d.high.process(heard);
                if (lowPassed) heard = d.low.process(heard);
                d.level = max(abs(heard), d.level * fall);
                loudest = max(loudest, d.level);
                if (!linked)
                    step(d, 20.0 * log10(max(d.level, 1e-10)), openDb, closeDb, holdFrames);
            }
            if (linked)
                step(detector[0], 20.0 * log10(max(loudest, 1e-10)), openDb, closeDb, holdFrames);
            for (int c = 0; c < n; ++c) {
                Detector& d = detector[linked ? 0 : c];
                if (!linked || c == 0) {
                    const double aim = d.open ? 1.0 : floorGain;
                    d.gain = aim + (aim > d.gain ? up : down) * (d.gain - aim);
                    opened[c][i] = d.open ? 1.0 : 0.0;
                    gain[c][i] = d.gain;
                }
                out[c][i] = d.gain * in[c][i];
            }
        }
    }

    void factoryPatches(Patches& p) {
        p.add("Clean Up")
            .set(threshold, -55)
            .set(hysteresis, 6)
            .set(holdTime, 80)
            .set(release, 250)
            .set(range, -18);
        p.add("Tight Drums")
            .set(threshold, -30)
            .set(hysteresis, 4)
            .set(attack, 0.1)
            .set(holdTime, 20)
            .set(release, 60);
        p.add("Kick Key")
            .set(threshold, -36)
            .set(attack, 0.2)
            .set(holdTime, 30)
            .set(release, 120)
            .set(keyLp, 150);
        p.add("Gated Verb")
            .set(threshold, -40)
            .set(hysteresis, 3)
            .set(holdTime, 250)
            .set(release, 20);
    }
};
