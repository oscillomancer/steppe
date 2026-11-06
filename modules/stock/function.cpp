// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A function generator, the Swiss army knife: a rise and a fall, each a
// time, bent by one curve. A trigger starts the rise from wherever it is
// (a trigger while rising is ignored, while falling it rises again); at the
// top it falls. Cycle makes it repeat on its own, an LFO whose shape is its
// rise, fall and curve. With a signal in In it follows that instead,
// rising no faster than Rise allows and falling no faster than Fall: a slew
// limiter, a portamento, and with Rectify an envelope follower. Speed moves
// both times at once (one cable for it all). End of Rise and End of Fall
// fire at the turns, to chain functions or clock other modules.

// Where a stage is after time t (0..1): a straight line at a curve of 0,
// fast first then settling towards 1, slow first then sudden towards -1.
double bend(double t, double curve) {
    const double a = 6.0 * curve;
    if (abs(a) < 1e-4) return t;
    return (1.0 - exp(-a * t)) / (1.0 - exp(-a));
}

// The time at which bend() reaches y.
double unbend(double y, double curve) {
    const double a = 6.0 * curve;
    if (abs(a) < 1e-4) return y;
    return -log(1.0 - y * (1.0 - exp(-a))) / a;
}

double width(double ms) { return 0.4 + log(1.0 + ms / 4.0); }

struct Function : Module {
    static constexpr ModuleInfo info = {
        .name = "Function",
        .id = "com.suppleaudio.rack.function",
        .vendor = "Supple Audio",
        .description = "Rise and fall: an envelope, a cycling LFO, a slew limiter or a follower.",
        .tags = {"envelope", "slew", "lfo", "modulation"}};

    Param rise = milliseconds("Rise", 0.5, 20000, 50)
                     .help(
                         "How long it takes to rise from 0 to 1 (as a slew: the fastest it may "
                         "climb a unit).");
    Param fall = milliseconds("Fall", 0.5, 20000, 400)
                     .help(
                         "How long it takes to fall from 1 to 0 (as a slew: the fastest it may "
                         "drop a unit).");
    Param curve =
        percent("Curve", 30, -100, 100)
            .help(
                "Positive moves fast first and settles (exponential); negative starts slowly and "
                "arrives suddenly (logarithmic); 0 is a straight line. As a slew, only the "
                "positive "
                "side applies: a lag.");
    Param speed =
        octaves("Speed", -4, 4, 0).help("Both times at once: each octave up halves them.");
    Param cycle = toggle("Cycle", false).help("Rise and fall over and over by itself, an LFO.");
    Param rectify = toggle("Rectify", false)
                        .help(
                            "Follow the input's size, not its sign: with a fast rise and slow "
                            "fall, an envelope "
                            "follower.");

    Input trigger =
        gateIn("Trigger").help("A rising edge starts the rise (rises at 0.5, falls below 0.25).");
    Input in = audioIn("In").help(
        "A signal to follow, as a slew limiter; the trigger and cycle wait while it is "
        "connected.");

    Output out = cvOut("Out").help(
        "The function: 0 to 1 as an envelope, the slewed input "
        "otherwise.");
    Output endOfRise = gateOut("End of Rise").help("A 1 ms trigger as it reaches the top.");
    Output endOfFall = gateOut("End of Fall").help("A 1 ms trigger as it reaches the bottom.");

    struct Voice {
        int stage = 0;  // 0 resting, 1 rising, 2 falling
        double t = 0.0;
        double from = 0.0;
        double level = 0.0;
        int direction = 0;  // as a slew: which way it last moved
        bool high = false;
        int risePulse = 0;
        int fallPulse = 0;
    };
    Array<Voice, 16> voices;
    int pulseFrames = 48;

    Shared<int> shownStage;
    Shared<double> shownT;
    Shared<double> shownLevel;

    void factoryPatches(Patches& p) {
        p.add("Percussive").set(rise, 2).set(fall, 300).set(curve, 60);
        p.add("Swell").set(rise, 1500).set(fall, 1500).set(curve, -40);
        p.add("Cycling LFO").set(cycle, true).set(rise, 700).set(fall, 700).set(curve, 0);
        p.add("Bouncing Ramp").set(cycle, true).set(rise, 900).set(fall, 30).set(curve, -60);
        p.add("Portamento").set(rise, 120).set(fall, 120).set(curve, 100);
        p.add("Envelope Follower").set(rectify, true).set(rise, 3).set(fall, 150).set(curve, 100);
    }

    void prepare() { pulseFrames = max(1, roundToInt(0.001 * sampleRate())); }

    void process(int frames) {
        const bool following = in.connected();
        const double msToStep = 1000.0 / sampleRate();
        for (int c = 0; c < channels(); ++c) {
            Voice& v = voices[c];
            for (int i = 0; i < frames; ++i) {
                const double k = clamp(curve.at(i, c), -1.0, 1.0);
                const double faster = exp2(speed.at(i, c));
                const double riseStep = msToStep * faster / max(0.5, rise.at(i, c));
                const double fallStep = msToStep * faster / max(0.5, fall.at(i, c));
                const double g = trigger[c][i];
                bool rose = false;
                if (!v.high && g >= 0.5) {
                    v.high = true;
                    rose = true;
                } else if (v.high && g < 0.25) {
                    v.high = false;
                }
                if (following) {
                    // A slew: straight at the fastest rate, or a lag with a
                    // positive curve.
                    const double x = in[c][i];
                    const double target = rectify.on() ? abs(x) : x;
                    const double d = target - v.level;
                    const double step = d > 0.0 ? riseStep : fallStep;
                    const double straight = clamp(d, -step, step);
                    const double lag = d * (1.0 - exp(-4.0 * step));
                    const double lean = max(0.0, k);
                    v.level += straight + (lag - straight) * lean;
                    const int dir = d > 1e-6 ? 1 : (d < -1e-6 ? -1 : 0);
                    if (v.direction == 1 && dir != 1) v.risePulse = pulseFrames;
                    if (v.direction == -1 && dir != -1) v.fallPulse = pulseFrames;
                    v.direction = dir;
                    v.stage = dir > 0 ? 1 : (dir < 0 ? 2 : 0);
                } else {
                    if ((rose || (cycle.on() && v.stage == 0)) && v.stage != 1) {
                        v.stage = 1;
                        v.t = unbend(clamp(v.level, 0.0, 1.0), k);
                    }
                    if (v.stage == 1) {
                        v.t += riseStep;
                        if (v.t >= 1.0) {
                            v.level = 1.0;
                            v.stage = 2;
                            v.t = 0.0;
                            v.from = 1.0;
                            v.risePulse = pulseFrames;
                        } else {
                            v.level = bend(v.t, k);
                        }
                    } else if (v.stage == 2) {
                        v.t += fallStep;
                        if (v.t >= 1.0) {
                            v.level = 0.0;
                            v.stage = 0;
                            v.fallPulse = pulseFrames;
                            if (cycle.on()) {
                                v.stage = 1;
                                v.t = 0.0;
                            }
                        } else {
                            v.level = v.from * (1.0 - bend(v.t, k));
                        }
                    }
                }
                out[c][i] = v.level;
                endOfRise[c][i] = v.risePulse > 0 ? 1.0 : 0.0;
                endOfFall[c][i] = v.fallPulse > 0 ? 1.0 : 0.0;
                if (v.risePulse > 0) --v.risePulse;
                if (v.fallPulse > 0) --v.fallPulse;
            }
        }
        shownStage.set(following ? 0 : voices[0].stage);
        shownT.set(voices[0].t);
        shownLevel.set(voices[0].level);
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(176, 264);
        p.display(8, 28, 160, 52);
        p.knob(rise, 22, 112);
        p.knob(fall, 66, 112);
        p.knob(curve, 110, 112);
        p.knob(speed, 154, 112);
        p.toggle(cycle, 8, 152, 76, 18);
        p.toggle(rectify, 92, 152, 76, 18);
        p.input(trigger, 22, 198);
        p.input(in, 66, 198);
        p.output(out, 66, 238);
        p.output(endOfRise, 110, 238);
        p.output(endOfFall, 154, 238);
    }

    void draw(Canvas& g) {
        g.well(8, 28, 160, 52);
        const double wr = width(rise.value());
        const double wf = width(fall.value());
        const double unit = 148.0 / (wr + wf);
        const double left = 14.0;
        const double bottom = 74.0;
        const double height = 40.0;
        const double k = clamp(curve.value(), -1.0, 1.0);
        const int n = 32;
        for (int pass = 0; pass < 2; ++pass) {
            g.beginPath();
            g.moveTo(left, bottom);
            for (int j = 1; j <= n; ++j) {
                const double u = static_cast<double>(j) / n;
                g.lineTo(left + u * wr * unit, bottom - height * bend(u, k));
            }
            for (int j = 1; j <= n; ++j) {
                const double u = static_cast<double>(j) / n;
                g.lineTo(left + (wr + u * wf) * unit, bottom - height * (1.0 - bend(u, k)));
            }
            if (pass == 0) {
                g.closePath();
                g.fillStyle(Theme::accent);
                g.globalAlpha(0.18);
                g.fill();
                g.globalAlpha(1.0);
            } else {
                g.strokeStyle(Theme::accent);
                g.lineWidth(1.6);
                g.lineJoin(LineJoin::Round);
                g.stroke();
            }
        }
        const int stage = shownStage.get();
        if (stage > 0) {
            const double t = clamp(shownT.get(), 0.0, 1.0);
            const double x = stage == 1 ? left + t * wr * unit : left + (wr + t * wf) * unit;
            g.fillStyle(Theme::text);
            g.beginPath();
            g.circle(x, bottom - height * clamp(shownLevel.get(), 0.0, 1.0), 3.0);
            g.fill();
        }
    }
};
