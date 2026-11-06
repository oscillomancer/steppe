// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A plucked string, poly: each channel a string of its own, tuned by Pitch and
// plucked by a trigger into Pluck (Velocity sets how hard). Decay is how long
// it rings, Damping how fast its highs fade (nylon to steel), Brightness the
// pick (a soft thumb to a sharp plectrum), Position where it is plucked (near
// the bridge thin and twangy, at the middle round and hollow), Stiffness how
// far its overtones stretch sharp, from a string towards a piano's wire and a
// struck bar. Exciter feeds any sound into the string instead (noise for a
// bowed drone, a drum loop for sympathetic ringing).
//
// Inside, a waveguide: one period of delay around a loop with a low pass (the
// damping), eight allpasses (the stiffness: their corner a few partials up,
// so the partials above it run round faster and sound sharp) and a gain (the
// decay), each
// filter's delay taken out of the line's so the pitch stays true. A pluck is
// a period of noise, low-passed by Brightness and combed by Position.

constexpr double kC4 = 261.6255653005986;
constexpr double kLowestHz = 20.0;

// A one-pole low pass y = (1 - a) x + a y' : its delay in samples at w.
double lowpassDelay(double a, double w) { return atan2(a * sin(w), 1.0 - a * cos(w)) / w; }
// A first-order allpass (c + z^-1) / (1 + c z^-1): its delay in samples at w.
double allpassDelay(double c, double w) {
    const double num = atan2(-sin(w), c + cos(w));
    const double den = atan2(-c * sin(w), 1.0 + c * cos(w));
    return -(num - den) / w;
}

struct Strand {
    DelayLine loop, pick;
    double lp = 0.0;            // the damping filter's state
    Array<double, 8> apX, apY;  // the allpasses' last input and output
    double burst = 0.0;         // samples of pluck left
    double force = 0.0;         // its velocity
    double soft = 0.0;          // the pick's low-pass state
    bool high = false;
    double dc = 0.0, dcIn = 0.0;  // the output's DC blocker
    Noise noise;
};

struct String : Module {
    static constexpr ModuleInfo info = {
        .name = "String",
        .id = "com.suppleaudio.rack.string",
        .vendor = "Supple Audio",
        .description =
            "A plucked string (a waveguide): pitch, damping, brightness, position, "
            "decay, stiffness.",
        .tags = {"physical-model", "source", "poly"}};

    Param decay = milliseconds("Decay", 50, 30000, 4000)
                      .help("How long the string rings, to 60 dB down at its fundamental.");
    Param damping =
        percent("Damping", 35)
            .help("How fast the highs die: bright steel at 0, a dull nylon thud at 100.");
    Param brightness =
        percent("Brightness", 60).help("The pick: a soft thumb at 0, a sharp plectrum at 100.");
    Param position = percent("Position", 20, 2, 50)
                         .help(
                             "Where it is plucked: near the bridge (low) twangs, the middle (50) "
                             "is round and hollow.");
    Param stiffness =
        percent("Stiffness", 0)
            .help("Stretches the overtones sharp: 0 is a string, 50 a piano's wire, 100 a bar.");
    Param level = decibels("Level", -24, 6, 0).smoothed(10).help("The output level.");

    Input pitch = pitchIn("Pitch").help("1 per octave; 0 is C4. Poly: a string per channel.");
    Input pluck = gateIn("Pluck").help("Plucks the string each time it rises.");
    Input velocity =
        unipolarIn("Velocity")
            .normal(1.0)
            .help("How hard each pluck is, 0 to 1, read as it rises; full while unconnected.");
    Input exciter = audioIn("Exciter").help(
        "Any sound fed into the string: noise bows it, a loop makes it ring along.");

    Output out = audioOut("Out").help("The string.");

    Array<Strand, 16> strand;

    void prepare() {
        const int longest = toInt(sampleRate() / kLowestHz) + 16;
        for (int c = 0; c < 16; ++c) {
            strand[c].loop.prepare(longest);
            strand[c].pick.prepare(longest);
            strand[c].noise.seed(seed() + 31 * c);
        }
    }

    void process(int frames) {
        const double sr = sampleRate();
        for (int c = 0; c < channels(); ++c) {
            Strand& s = strand[c];
            for (int i = 0; i < frames; ++i) {
                const double hz =
                    clamp(kC4 * exp2(clamp(pitch[c][i], -8.0, 6.0)), kLowestHz, 0.25 * sr);
                const double period = sr / hz;
                const double w = twoPi * hz / sr;
                // The loop's filters, and the delay they leave the line.
                const double damp = clamp(damping.at(i, c), 0.0, 1.0);
                const double a = 0.92 * damp * damp + 0.04;
                // Stiffness: the allpasses' corner from 50 partials up to 3.
                const double stiff = clamp(stiffness.at(i, c), 0.0, 1.0);
                const double corner = min(hz * 50.0 * pow(0.06, stiff), 0.2 * sr);
                const double t = tan(pi * corner / sr);
                const double ap = stiff > 0.0 ? (t - 1.0) / (t + 1.0) : 0.0;
                const double filters = lowpassDelay(a, w) + 8.0 * allpassDelay(ap, w);
                const double line = max(period - filters, 2.0);
                const double t60 = max(decay.at(i, c), 1.0) * 0.001 * sr;
                const double gain = pow(10.0, -3.0 * period / t60);
                // A pluck as Pluck rises: a period of noise.
                const double gate = pluck[c][i];
                if (!s.high && gate >= 0.5) {
                    s.high = true;
                    s.burst = period;
                    s.force = clamp(velocity[c][i], 0.0, 1.0);
                } else if (s.high && gate < 0.25) {
                    s.high = false;
                }
                double kick = 0.0;
                if (s.burst > 0.0) {
                    s.burst -= 1.0;
                    const double bright =
                        clamp(brightness.at(i, c) * (0.5 + 0.5 * s.force), 0.0, 1.0);
                    const double soften = 0.97 * pow(1.0 - bright, 1.5);
                    const double white = s.noise.white();
                    s.soft = (1.0 - soften) * white + soften * s.soft;
                    kick = s.soft * s.force;
                }
                s.pick.push(kick);
                const double at = clamp(position.at(i, c), 0.02, 0.5) * period;
                const double excite = kick - s.pick.read(at) + exciter[c][i] * 0.25;
                // Round the loop.
                double y = s.loop.read(line - 1.0);
                s.lp = (1.0 - a) * y + a * s.lp;
                y = s.lp;
                for (int k = 0; k < 8; ++k) {
                    const double z = ap * y + s.apX[k] - ap * s.apY[k];
                    s.apX[k] = y;
                    s.apY[k] = z;
                    y = z;
                }
                s.loop.push(y * gain + excite);
                const double o = y;
                s.dc = o - s.dcIn + 0.9995 * s.dc;
                s.dcIn = o;
                out[c][i] = 0.7 * s.dc * dbToGain(level.at(i, c));
            }
        }
    }
};
