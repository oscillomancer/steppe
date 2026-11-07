// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A lush stereo reverb: from a small bright room to an endless, shimmering
// cloud. Size sets the space, Decay how long it rings (Freeze, or a gate into
// Freeze Gate, holds it forever and lets nothing new in), Damping how fast
// the highs die, Diffusion how soon the echoes blur into a wash, Modulation a
// slow drift that keeps long tails from ringing metallic, and Shimmer feeds
// the tail back an octave up, climbing into a choir of overtones.
//
// Inside: a pre-delay, four allpass diffusers a side, then a feedback delay
// network of eight lines whose lengths share no common factor, mixed each
// pass by an 8 x 8 Hadamard matrix (which loses no energy, so the decay is
// set by each line's gain alone), each line damped by a low pass and its
// length swayed by a slow sine of its own. Shimmer's octave comes from two
// crossfaded taps sweeping through a short buffer.

constexpr int kLines = 8;
constexpr double kMaxPreMs = 500.0;
constexpr double kMaxScale = 1.5;
constexpr double kShiftWindow = 0.06;  // seconds
constexpr double kWet = 0.5;           // the tail's level out

// The lines' lengths at a scale of 1 (ms, no common factor) and their sway
// rates (Hz); the diffusers' lengths (ms), the left's then the right's.
double lineMs(int k) {
    switch (k) {
        case 0:
            return 31.3;
        case 1:
            return 37.9;
        case 2:
            return 43.1;
        case 3:
            return 49.7;
        case 4:
            return 56.3;
        case 5:
            return 63.1;
        case 6:
            return 71.3;
        default:
            return 79.9;
    }
}
double swayHz(int k) { return 0.11 + 0.07 * k + 0.003 * k * k; }
double diffuserMs(int k) {
    switch (k) {
        case 0:
            return 4.1;
        case 1:
            return 5.3;
        case 2:
            return 7.7;
        case 3:
            return 11.3;
        case 4:
            return 4.7;
        case 5:
            return 6.1;
        case 6:
            return 8.3;
        default:
            return 12.1;
    }
}

struct Diffuser {
    DelayLine line;
    double length = 1.0;  // samples

    double process(double x, double g) {
        const double old = line.read(length - 1.0);
        const double w = x + g * old;
        line.push(w);
        return old - g * w;
    }
};

struct Reverb : Module {
    static constexpr ModuleInfo info = {
        .name = "Reverb",
        .id = "com.suppleaudio.rack.reverb",
        .vendor = "Supple Audio",
        .description =
            "A lush stereo reverb: size, decay to freeze, damping, diffusion, "
            "modulation and shimmer.",
        .tags = {"reverb", "effect", "stereo"}};

    Param size = percent("Size", 60)
                     .in("Space")
                     .help("The space: a small, close room at 0, a hall at 60, a canyon at 100.");
    Param decay = milliseconds("Decay", 200, 30000, 3500)
                      .in("Space")
                      .help("How long the tail takes to fall 60 dB.");
    Param preDelay = milliseconds("Pre Delay", 0, 500, 12)
                         .in("Space")
                         .help("A gap before the reverb starts, keeping the dry attack clear.");
    Param damping = percent("Damping", 40)
                        .in("Tone")
                        .help(
                            "How quickly the highs fade in the tail: bright stone at 0, heavy "
                            "curtains at 100.");
    Param diffusion =
        percent("Diffusion", 70)
            .in("Tone")
            .help("How soon the echoes blur: distinct slaps at 0, an instant wash at 100.");
    Param modulation =
        percent("Modulation", 30)
            .in("Tone")
            .help("A slow sway of the lines that lushes the tail out and keeps it from ringing.");
    Param shimmer =
        percent("Shimmer", 0)
            .in("Tone")
            .help("Feeds the tail back an octave up: overtones that climb as it rings.");
    Param freeze = toggle("Freeze", false)
                       .in("Space")
                       .help("Holds the tail forever and lets no new sound in.");
    Param mix = percent("Mix", 30).smoothed(20).help(
        "Dry to wet: below 50 the dry stays full, above it the dry fades.");

    Input inL = audioIn("In L").help("The left input (poly cables are summed).");
    Input inR = audioIn("In R").help("The right input; the left feeds both while it is empty.");
    Input freezeGate = gateIn("Freeze Gate").help("Freezes the tail while high, as Freeze does.");

    Output outL = audioOut("Out L").help("The left output: dry and reverb.");
    Output outR = audioOut("Out R").help("The right output.");

    Array<DelayLine, kLines> line;
    Array<OnePole, kLines> damp;
    Array<double, kLines> base, length, rate, phase, v;
    Array<Diffuser, 8> diffuser;  // 0..3 the left's, 4..7 the right's
    DelayLine preL, preR, shiftBuffer;
    OnePole lowCutL, lowCutR, shiftCut;
    double shiftPhase = 0.0;
    double shimmerBack = 0.0;
    bool frozen = false;
    bool unplayed = true;

    void prepare() {
        const double sr = sampleRate();
        for (int k = 0; k < kLines; ++k) {
            base[k] = lineMs(k) * 0.001 * sr;
            line[k].prepare(toInt(base[k] * kMaxScale + 0.004 * sr) + 8);
            rate[k] = swayHz(k) / sr;
            phase[k] = 0.137 * k;
        }
        for (int k = 0; k < 8; ++k) {
            diffuser[k].length = diffuserMs(k) * 0.001 * sr;
            diffuser[k].line.prepare(toInt(diffuser[k].length) + 8);
        }
        const int pre = toInt(kMaxPreMs * 0.001 * sr) + 8;
        preL.prepare(pre);
        preR.prepare(pre);
        shiftBuffer.prepare(toInt(kShiftWindow * sr) + 8);
        lowCutL.setCutoff(20.0);
        lowCutR.setCutoff(20.0);
        shiftCut.setCutoff(150.0);
        unplayed = true;
    }

    // v = H v / sqrt(8), the Hadamard matrix by butterflies.
    void mixLines() {
        for (int span = 1; span < kLines; span = span * 2)
            for (int k = 0; k < kLines; k += 2 * span)
                for (int j = k; j < k + span; ++j) {
                    const double a = v[j], b = v[j + span];
                    v[j] = a + b;
                    v[j + span] = a - b;
                }
        for (int k = 0; k < kLines; ++k) v[k] *= 0.35355339059327373;
    }

    void process(int frames) {
        outL.setChannels(1);
        outR.setChannels(1);
        const double sr = sampleRate();
        const double sz = clamp(size.value(), 0.0, 1.0);
        const double scale = 0.15 + (kMaxScale - 0.15) * pow(sz, 1.3);
        const double dampHz = 20000.0 * pow(0.04, clamp(damping.value(), 0.0, 1.0));
        const double g = 0.75 * clamp(diffusion.value(), 0.0, 1.0);
        const double depth = 0.0012 * sr * clamp(modulation.value(), 0.0, 1.0);
        const double glide = 1.0 - exp(-1.0 / (0.2 * sr));
        const double window = kShiftWindow * sr;
        for (int k = 0; k < kLines; ++k)
            damp[k].setCutoff(frozen ? 0.45 * sr : min(dampHz, 0.45 * sr));
        for (int i = 0; i < frames; ++i) {
            frozen = freeze.on() || freezeGate[0][i] >= 0.5 || (frozen && freezeGate[0][i] >= 0.25);
            double l = 0.0, r = 0.0;
            for (int c = 0; c < max(inL.channels(), 1); ++c) l += inL[c][i];
            for (int c = 0; c < max(inR.channels(), 1); ++c) r += inR[c][i];
            if (!inR.connected()) r = l;
            // Pre-delay and diffusion.
            const double pre = clamp(preDelay.at(i, 0), 0.0, kMaxPreMs) * 0.001 * sr;
            preL.push(lowCutL.highpass(l));
            preR.push(lowCutR.highpass(r));
            double dl = preL.read(pre);
            double dr = preR.read(pre);
            for (int k = 0; k < 4; ++k) {
                dl = diffuser[k].process(dl, g);
                dr = diffuser[k + 4].process(dr, g);
            }
            if (frozen) {
                dl = 0.0;
                dr = 0.0;
            }
            // The network: read, damp, set the decay, mix, write back.
            const double t60 = max(decay.at(i, 0), 1.0) * 0.001 * sr;
            for (int k = 0; k < kLines; ++k) {
                const double aim = base[k] * scale;
                length[k] = unplayed ? aim : length[k] + (aim - length[k]) * glide;
                phase[k] += rate[k];
                phase[k] -= floor(phase[k]);
                const double at = length[k] + depth * (1.0 + sin(twoPi * phase[k]));
                const double y = line[k].read(at - 1.0);
                const double gain = frozen ? 1.0 : pow(10.0, -3.0 * at / t60);
                v[k] = (frozen ? y : damp[k].lowpass(y)) * gain;
            }
            unplayed = false;
            const double wetL = v[0] - v[2] + v[4] - v[6];
            const double wetR = v[1] - v[3] + v[5] - v[7];
            mixLines();
            const double back = frozen ? 0.0 : shimmerBack;
            for (int k = 0; k < kLines; ++k) {
                const double into = (k % 2 == 0 ? dl : dr) * 0.5 + back;
                // Linear for anything sensible; a long tail fed loud for
                // long saturates rather than climbing on.
                line[k].push(3.0 * tanh((v[k] + into) / 3.0));
            }
            // Shimmer: the tail an octave up, from two taps sweeping the
            // buffer at twice speed, each faded in and out by a sine squared.
            shiftBuffer.push(0.5 * (wetL + wetR));
            shiftPhase += 1.0 / window;
            shiftPhase -= floor(shiftPhase);
            const double p2 = shiftPhase + 0.5 - floor(shiftPhase + 0.5);
            const double a = sin(pi * shiftPhase), b = sin(pi * p2);
            const double up = a * a * shiftBuffer.read(window * (1.0 - shiftPhase)) +
                              b * b * shiftBuffer.read(window * (1.0 - p2));
            const double amount = clamp(shimmer.at(i, 0), 0.0, 1.0);
            shimmerBack = 0.35 * amount * tanh(shiftCut.highpass(up));
            const double m = mix.at(i, 0);
            const double dry = min(1.0, 2.0 - 2.0 * m), wet = min(1.0, 2.0 * m);
            // The network's tail, halved, comes back about as loud as what
            // goes in at the default decay; it builds up with longer ones.
            outL[0][i] = dry * l + wet * kWet * wetL;
            outR[0][i] = dry * r + wet * kWet * wetR;
        }
    }
};
