// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// An envelope: attack, decay, sustain and release, each stage with a curve
// of its own, poly. Gated, it holds the sustain while the gate is high, as
// a keyboard's envelope; triggered, the whole shape plays from any pulse;
// looping, it cycles while the gate is high (or forever with nothing in the
// gate), a slow and shapely LFO. A retrigger restarts the attack from where
// the envelope is, so a legato line can still articulate. Velocity scales
// each shape as it starts. Patch the envelope into a VCA's CV or a filter's
// cutoff; the end trigger fires as each shape dies away, to chain another.

// Where a stage is, 0..1 in time, on its way, 0..1: a straight line at a
// curve of 0, fast first then settling (as a capacitor charges) towards 1,
// slow first then sudden towards -1.
double bend(double t, double curve) {
    const double a = 6.0 * curve;
    if (abs(a) < 1e-4) return t;
    return (1.0 - exp(-a * t)) / (1.0 - exp(-a));
}

// The time at which bend() reaches y: where an attack resumes from a level.
double unbend(double y, double curve) {
    const double a = 6.0 * curve;
    if (abs(a) < 1e-4) return y;
    return -log(1.0 - y * (1.0 - exp(-a))) / a;
}

// The width a stage of `ms` takes in the display: long times grow slowly.
double span(double ms) { return 0.4 + log(1.0 + ms / 4.0); }

enum class Stage { Idle, Attack, Decay, Sustain, Release };

struct Envelope : Module {
    static constexpr ModuleInfo info = {
        .name = "ADSR",
        .id = "com.suppleaudio.rack.adsr",
        .vendor = "Supple Audio",
        .description = "An envelope with curved stages: gated, triggered or looping, poly.",
        .tags = {"envelope", "modulation", "poly"}};

    Param attack = milliseconds("Attack", 0.5, 20000, 5)
                       .in("Envelope")
                       .help("How long it takes to rise from 0 to the top.");
    Param decay = milliseconds("Decay", 1, 20000, 300)
                      .in("Envelope")
                      .help("How long it takes to fall from the top to the sustain level.");
    Param sustain = percent("Sustain", 60)
                        .smoothed(5)
                        .in("Envelope")
                        .help("The level held while the gate is high, as a share of the top.");
    Param release = milliseconds("Release", 1, 20000, 400)
                        .in("Envelope")
                        .help("How long it takes to fall to 0 once the gate is low.");
    Param attackCurve =
        percent("A Curve", 20, -100, 100)
            .in("Curves")
            .help(
                "Positive rises fast and eases into the top, as an analog envelope; negative "
                "starts slowly and arrives suddenly; 0 is a straight line.");
    Param decayCurve =
        percent("D Curve", 60, -100, 100)
            .in("Curves")
            .help(
                "Positive drops fast and settles into the sustain (natural for plucks); negative "
                "holds near the top, then falls.");
    Param releaseCurve =
        percent("R Curve", 60, -100, 100)
            .in("Curves")
            .help(
                "Positive fades fast then lingers, as a sound dies away; negative holds, then "
                "cuts off.");
    Param mode = choice("Mode", {"Gated", "Triggered", "Looping"}, 0)
                     .help(
                         "Gated holds the sustain while the gate is high; triggered plays "
                         "the whole shape from any pulse; looping repeats it while the gate "
                         "is high, or always with nothing in the gate.");

    Input gate = gateIn("Gate").help(
        "High starts the attack (rises at 0.5, falls below 0.25); low starts the release.");
    Input retrigger = gateIn("Retrig").help(
        "A rising edge restarts the attack from the present level (while the gate is high, "
        "when gated).");
    Input velocity =
        unipolarIn("Velocity")
            .normal(1.0)
            .help("The top of each shape, 0 to 1, taken as it starts; 1 with nothing connected.");

    Output envelope = unipolarOut("Envelope").help("The envelope, 0 to its top.");
    Output inverted =
        unipolarOut("Inverted").help("1 minus the envelope: it closes as the envelope opens.");
    Output endOfCycle =
        gateOut("End").help("A 1 ms trigger as each shape reaches 0 (each cycle, when looping).");

    struct Voice {
        Stage stage = Stage::Idle;
        double t = 0.0;      // through the stage, 0..1
        double from = 0.0;   // the level the stage started at (decay, release)
        double level = 0.0;  // the output
        double top = 1.0;    // velocity, taken at the start
        bool gateHigh = false;
        bool retrigHigh = false;
        int pulse = 0;  // frames of the end trigger still to send
    };
    Array<Voice, 16> voices;
    int pulseFrames = 48;

    Shared<double> shownLevel;
    Shared<int> shownStage;
    Shared<double> shownT;

    void factoryPatches(Patches& p) {
        p.add("Pluck")
            .set(attack, 1)
            .set(decay, 250)
            .set(sustain, 0)
            .set(release, 250)
            .set(decayCurve, 70);
        p.add("Pad").set(attack, 900).set(decay, 1500).set(sustain, 70).set(release, 2500);
        p.add("Organ").set(attack, 2).set(decay, 10).set(sustain, 100).set(release, 20);
        p.add("Swell")
            .set(attack, 3000)
            .set(attackCurve, -60)
            .set(decay, 20)
            .set(sustain, 100)
            .set(release, 1200);
        p.add("Drum Hit")
            .set(mode, "triggered")
            .set(attack, 0.5)
            .set(decay, 120)
            .set(sustain, 0)
            .set(release, 10)
            .set(decayCurve, 80);
        p.add("Breathing Loop")
            .set(mode, "looping")
            .set(attack, 1800)
            .set(attackCurve, -30)
            .set(decay, 2400)
            .set(sustain, 0)
            .set(decayCurve, 30)
            .set(release, 10);
    }

    void prepare() { pulseFrames = max(1, roundToInt(0.001 * sampleRate())); }

    // Starts (or restarts) the attack from the present level.
    void startAttack(Voice& v, double top, double curve) {
        v.top = clamp(top, 0.0, 1.0);
        v.stage = Stage::Attack;
        v.t = v.top > 0.0 ? unbend(clamp(v.level / v.top, 0.0, 1.0), curve) : 1.0;
    }

    void startRelease(Voice& v) {
        v.stage = Stage::Release;
        v.from = v.level;
        v.t = 0.0;
    }

    void process(int frames) {
        const int m = mode.index();
        const bool running = m == 2 && !gate.connected();
        const double msToStep = 1000.0 / sampleRate();
        for (int c = 0; c < channels(); ++c) {
            Voice& v = voices[c];
            for (int i = 0; i < frames; ++i) {
                const double ka = attackCurve.at(i, c);
                const double kd = decayCurve.at(i, c);
                const double kr = releaseCurve.at(i, c);
                const double s = clamp(sustain.at(i, c), 0.0, 1.0);
                // The gate, with hysteresis; a looping envelope with nothing
                // in its gate runs on its own.
                const double g = gate[c][i];
                bool rose = false;
                bool fell = false;
                if (!v.gateHigh && g >= 0.5) {
                    v.gateHigh = true;
                    rose = true;
                } else if (v.gateHigh && g < 0.25) {
                    v.gateHigh = false;
                    fell = true;
                }
                const double r = retrigger[c][i];
                bool retrig = false;
                if (!v.retrigHigh && r >= 0.5) {
                    v.retrigHigh = true;
                    retrig = m != 0 || v.gateHigh;
                } else if (v.retrigHigh && r < 0.25) {
                    v.retrigHigh = false;
                }
                if (rose || retrig || (running && v.stage == Stage::Idle))
                    startAttack(v, velocity[c][i], ka);
                if (fell && m == 0 && v.stage != Stage::Idle) startRelease(v);

                if (v.stage == Stage::Attack) {
                    v.t += msToStep / max(0.5, attack.at(i, c));
                    if (v.t >= 1.0) {
                        v.level = v.top;
                        v.stage = Stage::Decay;
                        v.from = v.top;
                        v.t = 0.0;
                    } else {
                        v.level = v.top * bend(v.t, ka);
                    }
                } else if (v.stage == Stage::Decay) {
                    const double target = s * v.top;
                    v.t += msToStep / max(1.0, decay.at(i, c));
                    if (v.t >= 1.0) {
                        v.level = target;
                        if (m == 0) {
                            v.stage = Stage::Sustain;
                        } else {
                            startRelease(v);
                        }
                    } else {
                        v.level = target + (v.from - target) * (1.0 - bend(v.t, kd));
                    }
                } else if (v.stage == Stage::Sustain) {
                    v.level = s * v.top;
                } else if (v.stage == Stage::Release) {
                    v.t += msToStep / max(1.0, release.at(i, c));
                    if (v.t >= 1.0) {
                        v.level = 0.0;
                        v.stage = Stage::Idle;
                        v.pulse = pulseFrames;
                        if (m == 2 && (v.gateHigh || running)) startAttack(v, velocity[c][i], ka);
                    } else {
                        v.level = v.from * (1.0 - bend(v.t, kr));
                    }
                }
                envelope[c][i] = v.level;
                inverted[c][i] = 1.0 - v.level;
                endOfCycle[c][i] = v.pulse > 0 ? 1.0 : 0.0;
                if (v.pulse > 0) --v.pulse;
            }
        }
        const Voice& first = voices[0];
        shownLevel.set(first.level);
        shownStage.set(first.stage == Stage::Idle      ? 0
                       : first.stage == Stage::Attack  ? 1
                       : first.stage == Stage::Decay   ? 2
                       : first.stage == Stage::Sustain ? 3
                                                       : 4);
        shownT.set(first.t);
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(176, 314);
        p.display(8, 28, 160, 64);
        p.knob(attack, 22, 120);
        p.knob(decay, 66, 120);
        p.knob(sustain, 110, 120);
        p.knob(release, 154, 120);
        p.knob(attackCurve, 22, 174, 24);
        p.knob(decayCurve, 66, 174, 24);
        p.knob(releaseCurve, 154, 174, 24);
        p.choice(mode, 8, 206, 160, 18);
        p.input(gate, 22, 246);
        p.input(retrigger, 66, 246);
        p.input(velocity, 110, 246);
        p.output(envelope, 66, 286);
        p.output(inverted, 110, 286);
        p.output(endOfCycle, 154, 286);
    }

    void draw(Canvas& g) {
        const double x0 = 8.0;
        const double y0 = 28.0;
        const double w = 160.0;
        const double h = 64.0;
        g.well(x0, y0, w, h);
        const bool gated = mode.index() == 0;
        const double s = clamp(sustain.value(), 0.0, 1.0);
        const double wa = span(attack.value());
        const double wd = span(decay.value());
        const double ws = gated ? 2.5 : 0.0;
        const double wr = span(release.value());
        const double unit = (w - 12.0) / (wa + wd + ws + wr);
        const double bottom = y0 + h - 5.0;
        const double height = h - 12.0;
        const double xa = x0 + 6.0;
        const double xd = xa + wa * unit;
        const double xs = xd + wd * unit;
        const double xr = xs + ws * unit;
        const double xe = xr + wr * unit;
        const int n = 24;

        // Stage boundaries, faint.
        g.strokeStyle(Theme::line);
        g.lineWidth(1.0);
        g.beginPath();
        g.moveTo(xd, y0 + 4);
        g.lineTo(xd, bottom);
        g.moveTo(xs, y0 + 4);
        g.lineTo(xs, bottom);
        if (gated) {
            g.moveTo(xr, y0 + 4);
            g.lineTo(xr, bottom);
        }
        g.stroke();

        // The shape, filled beneath and stroked.
        for (int pass = 0; pass < 2; ++pass) {
            g.beginPath();
            g.moveTo(xa, bottom);
            for (int k = 1; k <= n; ++k) {
                const double u = static_cast<double>(k) / n;
                g.lineTo(xa + u * wa * unit, bottom - height * bend(u, attackCurve.value()));
            }
            for (int k = 1; k <= n; ++k) {
                const double u = static_cast<double>(k) / n;
                const double lev = s + (1.0 - s) * (1.0 - bend(u, decayCurve.value()));
                g.lineTo(xd + u * wd * unit, bottom - height * lev);
            }
            g.lineTo(xr, bottom - height * s);
            for (int k = 1; k <= n; ++k) {
                const double u = static_cast<double>(k) / n;
                const double lev = s * (1.0 - bend(u, releaseCurve.value()));
                g.lineTo(xr + u * wr * unit, bottom - height * lev);
            }
            if (pass == 0) {
                g.lineTo(xe, bottom);
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

        // Where the first channel is now.
        const int stage = shownStage.get();
        if (stage > 0) {
            const double t = clamp(shownT.get(), 0.0, 1.0);
            const double x = stage == 1   ? xa + t * wa * unit
                             : stage == 2 ? xd + t * wd * unit
                             : stage == 3 ? (xs + xr) * 0.5
                                          : xr + t * wr * unit;
            const double y = bottom - height * clamp(shownLevel.get(), 0.0, 1.0);
            g.fillStyle(Theme::text);
            g.beginPath();
            g.circle(x, y, 3.0);
            g.fill();
        }
    }
};
