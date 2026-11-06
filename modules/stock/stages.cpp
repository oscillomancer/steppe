// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A function generator of two to six stages, each moving to its Level over
// its Time along its Curve: an envelope with as many segments as it needs,
// an LFO of any shape, or a one-shot gesture. Mode says what a gate does:
// Once runs every stage on each rise and stops at the last level; Loop
// cycles round and round (a rise starts it over); Gated runs to the end of
// the Hold stage and stays there while the gate is high, then goes on from
// wherever it is when the gate falls (an envelope with a sustain anywhere).
// Each stage starts from where the output is, so a retrigger never jumps.
// Speed scales every time at once. Stage 1 to 6 are high while their stage
// runs; End fires as the last stage ends. Poly: a channel for each gate.

struct Stages : Module {
    static constexpr ModuleInfo info = {
        .name = "Stages",
        .id = "com.suppleaudio.rack.stages",
        .vendor = "Supple Audio",
        .description = "A function generator of up to six stages: envelope, LFO or gesture.",
        .tags = {"envelope", "modulation", "poly"}};

    Param mode = choice("Mode", {"Once", "Loop", "Gated"}, 2)
                     .help(
                         "Once: every stage on each rise. Loop: round and round. Gated: holds at "
                         "the Hold stage while the gate is high.");
    Param count = integer("Stages", 2, 6, 4).help("How many stages run.");
    Param hold = integer("Hold", 1, 5, 3)
                     .activeWhen(mode, {"gated"})
                     .help("The stage whose level holds while the gate is high (Gated).");
    Param speed = ratio("Speed", 0.1, 10, 1).help("Every stage's time divided by this.");
    Param level_1 = percent("Level 1", 100, -100, 100)
                        .in("Stage 1")
                        .help("Where stage 1 ends: the level it moves to.");
    Param time_1 = milliseconds("Time 1", 1, 30000, 10)
                       .in("Stage 1")
                       .help("How long stage 1 takes, before Speed.");
    Param curve_1 = percent("Curve 1", 50, -100, 100)
                        .in("Stage 1")
                        .help("Above 0 stage 1 starts fast and settles; below, starts slow.");
    Param level_2 = percent("Level 2", 60, -100, 100)
                        .in("Stage 2")
                        .help("Where stage 2 ends: the level it moves to.");
    Param time_2 = milliseconds("Time 2", 1, 30000, 200)
                       .in("Stage 2")
                       .help("How long stage 2 takes, before Speed.");
    Param curve_2 = percent("Curve 2", 50, -100, 100)
                        .in("Stage 2")
                        .help("Above 0 stage 2 starts fast and settles; below, starts slow.");
    Param level_3 = percent("Level 3", 60, -100, 100)
                        .in("Stage 3")
                        .help("Where stage 3 ends: the level it moves to.");
    Param time_3 = milliseconds("Time 3", 1, 30000, 300)
                       .in("Stage 3")
                       .help("How long stage 3 takes, before Speed.");
    Param curve_3 = percent("Curve 3", 0, -100, 100)
                        .in("Stage 3")
                        .help("Above 0 stage 3 starts fast and settles; below, starts slow.");
    Param level_4 = percent("Level 4", 0, -100, 100)
                        .in("Stage 4")
                        .help("Where stage 4 ends: the level it moves to.");
    Param time_4 = milliseconds("Time 4", 1, 30000, 500)
                       .in("Stage 4")
                       .help("How long stage 4 takes, before Speed.");
    Param curve_4 = percent("Curve 4", 50, -100, 100)
                        .in("Stage 4")
                        .help("Above 0 stage 4 starts fast and settles; below, starts slow.");
    Param level_5 = percent("Level 5", 30, -100, 100)
                        .in("Stage 5")
                        .help("Where stage 5 ends: the level it moves to.");
    Param time_5 = milliseconds("Time 5", 1, 30000, 300)
                       .in("Stage 5")
                       .help("How long stage 5 takes, before Speed.");
    Param curve_5 = percent("Curve 5", 0, -100, 100)
                        .in("Stage 5")
                        .help("Above 0 stage 5 starts fast and settles; below, starts slow.");
    Param level_6 = percent("Level 6", 0, -100, 100)
                        .in("Stage 6")
                        .help("Where stage 6 ends: the level it moves to.");
    Param time_6 = milliseconds("Time 6", 1, 30000, 300)
                       .in("Stage 6")
                       .help("How long stage 6 takes, before Speed.");
    Param curve_6 = percent("Curve 6", 0, -100, 100)
                        .in("Stage 6")
                        .help("Above 0 stage 6 starts fast and settles; below, starts slow.");

    Input gate = gateIn("Gate").help(
        "A rise starts stage 1 (rises at 0.5, falls below 0.25); poly. Loop runs without it.");

    Output out = cvOut("Out").help("The stages' output, -1 to 1 at full levels.");
    Output end = gateOut("End").help("A trigger as the last stage ends.");
    Outputs<6> stage = gateOut("Stage").help("High while its stage runs.");

    struct Voice {
        Trigger edge;
        Pulse endPulse;
        int at = -1;        // the stage running, -1 none
        double t = 0.0;     // how far through it, 0..1
        double from = 0.0;  // where it started
        double value = 0.0;
    };
    Array<Voice, 16> voices;

    Shared<int> shownStage;
    Shared<float> shownT;

    void factoryPatches(Patches& p) {
        p.add("Pluck")
            .set(mode, "once")
            .set(count, 2)
            .set(time_1, 2)
            .set(time_2, 400)
            .set(level_2, 0)
            .set(curve_2, 70);
        p.add("Swell and Fall")
            .set(mode, "gated")
            .set(count, 3)
            .set(hold, 2)
            .set(time_1, 1500)
            .set(curve_1, -50)
            .set(level_2, 80)
            .set(time_2, 300)
            .set(level_3, 0)
            .set(time_3, 2000);
        p.add("Step LFO")
            .set(mode, "loop")
            .set(count, 4)
            .set(level_1, 100)
            .set(time_1, 1)
            .set(level_2, 100)
            .set(time_2, 250)
            .set(level_3, -100)
            .set(time_3, 1)
            .set(level_4, -100)
            .set(time_4, 250);
        p.add("Bouncing Gesture")
            .set(mode, "once")
            .set(count, 6)
            .set(level_1, 100)
            .set(time_1, 300)
            .set(curve_1, -60)
            .set(level_2, 0)
            .set(time_2, 300)
            .set(curve_2, 60)
            .set(level_3, 50)
            .set(time_3, 150)
            .set(level_4, 0)
            .set(time_4, 150)
            .set(level_5, 20)
            .set(time_5, 80)
            .set(level_6, 0)
            .set(time_6, 80);
        p.add("Slow Rise LFO")
            .set(mode, "loop")
            .set(count, 2)
            .set(level_1, 100)
            .set(time_1, 3000)
            .set(curve_1, -40)
            .set(level_2, -100)
            .set(time_2, 600)
            .set(curve_2, 40);
    }

    double levelAt(int k, int i, int c) const {
        switch (k) {
            case 0:
                return level_1.at(i, c);
            case 1:
                return level_2.at(i, c);
            case 2:
                return level_3.at(i, c);
            case 3:
                return level_4.at(i, c);
            case 4:
                return level_5.at(i, c);
            default:
                return level_6.at(i, c);
        }
    }

    double timeAt(int k, int i, int c) const {
        switch (k) {
            case 0:
                return time_1.at(i, c);
            case 1:
                return time_2.at(i, c);
            case 2:
                return time_3.at(i, c);
            case 3:
                return time_4.at(i, c);
            case 4:
                return time_5.at(i, c);
            default:
                return time_6.at(i, c);
        }
    }

    double curveAt(int k, int i, int c) const {
        switch (k) {
            case 0:
                return curve_1.at(i, c);
            case 1:
                return curve_2.at(i, c);
            case 2:
                return curve_3.at(i, c);
            case 3:
                return curve_4.at(i, c);
            case 4:
                return curve_5.at(i, c);
            default:
                return curve_6.at(i, c);
        }
    }

    double levelShown(int k) const {
        switch (k) {
            case 0:
                return level_1.value();
            case 1:
                return level_2.value();
            case 2:
                return level_3.value();
            case 3:
                return level_4.value();
            case 4:
                return level_5.value();
            default:
                return level_6.value();
        }
    }

    double timeShown(int k) const {
        switch (k) {
            case 0:
                return time_1.value();
            case 1:
                return time_2.value();
            case 2:
                return time_3.value();
            case 3:
                return time_4.value();
            case 4:
                return time_5.value();
            default:
                return time_6.value();
        }
    }

    double curveShown(int k) const {
        switch (k) {
            case 0:
                return curve_1.value();
            case 1:
                return curve_2.value();
            case 2:
                return curve_3.value();
            case 3:
                return curve_4.value();
            case 4:
                return curve_5.value();
            default:
                return curve_6.value();
        }
    }

    // How far along a stage's move, for t from 0 to 1 and a curve of -1..1.
    double shaped(double t, double bend) const {
        const double e = 1.0 + 4.0 * abs(bend);
        if (bend > 0.0) return 1.0 - pow(1.0 - t, e);
        if (bend < 0.0) return pow(t, e);
        return t;
    }

    void begin(Voice& v, int k) {
        v.at = k;
        v.t = 0.0;
        v.from = v.value;
    }

    void process(int frames) {
        const int n = max(1, channels());
        out.setChannels(n);
        end.setChannels(n);
        for (int k = 0; k < 6; ++k) stage[k].setChannels(n);
        const int stages = clamp(count.index(), 2, 6);
        const int how = mode.index();
        const int held = clamp(hold.index(), 1, stages - 1) - 1;
        const double msPerFrame = 1000.0 / sampleRate();
        for (int i = 0; i < frames; ++i) {
            for (int c = 0; c < n; ++c) {
                Voice& v = voices[c];
                const bool wasHigh = v.edge.high();
                const bool rose = v.edge.rose(gate[c][i]);
                const bool high = v.edge.high();
                if (rose) begin(v, 0);
                if (how == 1 && v.at < 0) begin(v, 0);
                // Gated: the gate falling moves on past the Hold stage.
                if (how == 2 && wasHigh && !high && v.at >= 0 && v.at <= held) begin(v, held + 1);
                if (v.at >= stages) v.at = -1;  // Stages turned down under it
                if (v.at >= 0) {
                    const bool sustain = how == 2 && v.at == held && high;
                    v.t += msPerFrame * clamp(speed.at(i, c), 0.1, 10.0) /
                           max(1.0, timeAt(v.at, i, c));
                    if (v.t >= 1.0 && sustain) {
                        v.t = 1.0;
                    } else if (v.t >= 1.0) {
                        v.value = clamp(levelAt(v.at, i, c), -1.0, 1.0);
                        if (v.at + 1 < stages) {
                            begin(v, v.at + 1);
                        } else {
                            v.endPulse.fire();
                            v.at = -1;
                            if (how == 1) begin(v, 0);
                        }
                    }
                    if (v.at >= 0) {
                        const double to = clamp(levelAt(v.at, i, c), -1.0, 1.0);
                        const double bend = clamp(curveAt(v.at, i, c), -1.0, 1.0);
                        v.value = v.from + (to - v.from) * shaped(v.t, bend);
                    }
                }
                out[c][i] = v.value;
                end[c][i] = v.endPulse.next();
                for (int k = 0; k < 6; ++k) stage[k][c][i] = v.at == k ? 1.0 : 0.0;
            }
        }
        shownStage.set(voices[0].at);
        shownT.set(static_cast<float>(voices[0].t));
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(280, 424);
        p.display(8, 28, 264, 92);
        p.choice(mode, 8, 128, 264, 18);
        p.knob(count, 30, 180);
        p.knob(hold, 74, 180);
        p.knob(speed, 118, 180);
        p.input(gate, 162, 180);
        p.output(out, 206, 180);
        p.output(end, 250, 180);
        p.knob(level_1, 30, 238, 28);
        p.knob(time_1, 30, 290, 28);
        p.knob(curve_1, 30, 342, 28);
        p.knob(level_2, 74, 238, 28);
        p.knob(time_2, 74, 290, 28);
        p.knob(curve_2, 74, 342, 28);
        p.knob(level_3, 118, 238, 28);
        p.knob(time_3, 118, 290, 28);
        p.knob(curve_3, 118, 342, 28);
        p.knob(level_4, 162, 238, 28);
        p.knob(time_4, 162, 290, 28);
        p.knob(curve_4, 162, 342, 28);
        p.knob(level_5, 206, 238, 28);
        p.knob(time_5, 206, 290, 28);
        p.knob(curve_5, 206, 342, 28);
        p.knob(level_6, 250, 238, 28);
        p.knob(time_6, 250, 290, 28);
        p.knob(curve_6, 250, 342, 28);
        for (int k = 0; k < 6; ++k) p.output(stage[k], 30 + 44 * k, 396);
    }

    // The shape the knobs make, each stage as wide as the root of its time,
    // the stage running lit and a dot where it is.
    void draw(Canvas& g) {
        const double left = 8.0;
        const double top = 28.0;
        const double w = 264.0;
        const double h = 92.0;
        g.well(left, top, w, h);
        const int stages = clamp(count.index(), 2, 6);
        const int how = mode.index();
        const int held = clamp(hold.index(), 1, stages - 1) - 1;
        const int now = shownStage.get();
        const double nowT = static_cast<double>(shownT.get());
        double total = 0.0;
        for (int k = 0; k < stages; ++k) total += sqrt(max(1.0, timeShown(k)));
        const double x0 = left + 8.0;
        const double span = w - 16.0;
        // Bipolar only when a level is below 0 (or a loop starts from one).
        bool below = false;
        for (int k = 0; k < stages; ++k)
            if (levelShown(k) < 0.0) below = true;
        const double mid = below ? top + h / 2.0 : top + h - 8.0;
        const double half = below ? h / 2.0 - 8.0 : h - 16.0;
        double x = x0;
        double from = how == 1 ? clamp(levelShown(stages - 1), -1.0, 1.0) : 0.0;
        double dotX = -1.0;
        double dotY = 0.0;
        g.lineJoin(LineJoin::Round);
        for (int k = 0; k < stages; ++k) {
            const double sw = span * sqrt(max(1.0, timeShown(k))) / total;
            const double to = clamp(levelShown(k), -1.0, 1.0);
            const double bend = clamp(curveShown(k), -1.0, 1.0);
            if (k == now) {
                g.fillStyle(Theme::recess);
                g.fillRect(x, top + 3.0, sw, h - 6.0);
            }
            g.strokeStyle(k == now ? Theme::text : Theme::accent);
            g.lineWidth(k == now ? 2.0 : 1.5);
            g.beginPath();
            for (int q = 0; q <= 24; ++q) {
                const double t = q / 24.0;
                const double y = mid - (from + (to - from) * shaped(t, bend)) * half;
                if (q == 0)
                    g.moveTo(x + sw * t, y);
                else
                    g.lineTo(x + sw * t, y);
            }
            g.stroke();
            if (k == now) {
                dotX = x + sw * clamp(nowT, 0.0, 1.0);
                dotY = mid - (from + (to - from) * shaped(clamp(nowT, 0.0, 1.0), bend)) * half;
            }
            if (how == 2 && k == held) {
                g.strokeStyle(Theme::accent2);
                g.lineWidth(1.0);
                g.beginPath();
                g.moveTo(x + sw, top + 6.0);
                g.lineTo(x + sw, top + h - 6.0);
                g.stroke();
            }
            x += sw;
            from = to;
        }
        g.globalAlpha(0.6);
        g.strokeStyle(Theme::line);
        g.lineWidth(1.0);
        g.beginPath();
        g.moveTo(x0, mid);
        g.lineTo(x0 + span, mid);
        g.stroke();
        g.globalAlpha(1.0);
        if (dotX >= 0.0) {
            g.fillStyle(Theme::text);
            g.beginPath();
            g.circle(dotX, dotY, 3.0);
            g.fill();
        }
    }
};
