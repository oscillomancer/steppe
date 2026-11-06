// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A ball dropped on a hard floor: it falls for Fall, hits, and rises again
// with Bounce of its speed, so the hits come quicker and softer until it
// settles: the accelerating patter of a dropped ball or a ping-pong rally,
// as a rhythm. Hit fires at each impact and Strength holds how hard it was
// (the first 1, then Bounce, Bounce squared ...), for an accent or a
// velocity; Height is the ball's height, 0 to 1, a curve of arcs to
// modulate with; Air is high while it flies; End fires as it settles.
// Rough makes the floor uneven: each bounce keeps a little more or less.
// With Loop on it is dropped again as it settles (and from the start);
// Drop drops it at any time. Poly: a ball a channel. Reset drops it again
// with the same roughness.

struct Bounce : Module {
    static constexpr ModuleInfo info = {
        .name = "Bounce",
        .id = "com.suppleaudio.rack.bounce",
        .vendor = "Supple Audio",
        .description = "A bouncing ball: hits that come quicker and softer until it settles.",
        .tags = {"generative", "rhythm", "modulation", "poly"}};

    Param fall = milliseconds("Fall", 20, 4000, 500)
                     .help("How long the first fall takes, to the first hit.");
    Param bounce = percent("Bounce", 75, 0, 99)
                       .help(
                           "How much of its speed the ball keeps at each hit: high bounces long, "
                           "low settles soon.");
    Param rough = percent("Rough", 0)
                      .help("How uneven the floor is: each bounce keeps a little more or less.");
    Param loop = toggle("Loop", true).help("Drop the ball again as it settles, and at the start.");

    Input drop = gateIn("Drop").help("A rising edge drops the ball from the top; poly.");
    Input reset = gateIn("Reset").help("A rising edge drops it again with the same roughness.");

    Output hit = gateOut("Hit").help("A trigger at each impact.");
    Output end = gateOut("End").help("A trigger as the ball settles.");
    Output height = unipolarOut("Height").help("The ball's height, 1 at the top of the drop.");
    Output strength =
        unipolarOut("Strength")
            .help("How hard the last hit was, held to the next: 1 for the first, then softer.");
    Output air = gateOut("Air").help("High while the ball is in the air.");

    struct Ball {
        Trigger edge;
        Pulse hitPulse;
        Pulse endPulse;
        double u = 0.0;  // launch speed, 1 for a drop from the top
        double s = 0.0;  // time into this flight, in falls
        double last = 0.0;
        double since = 0.0;  // falls since the drop, for the face
        bool flying = false;
    };
    Array<Ball, 16> balls;
    Random rng;
    Trigger resetEdge;
    int salt = 0;
    bool started = false;

    Shared<float> shownHeight;
    Shared<float> shownSince;
    Shared<int> shownFlying;

    void factoryPatches(Patches& p) {
        p.add("Ping Pong").set(fall, 250).set(bounce, 88);
        p.add("Dead Drop").set(fall, 400).set(bounce, 40);
        p.add("Rubber Ball").set(fall, 900).set(bounce, 92);
        p.add("Gravel").set(fall, 300).set(bounce, 70).set(rough, 60);
        p.add("One Shot").set(loop, false);
    }

    void prepare() {
        salt = seed();
        rng.seed(salt);
        started = false;
    }

    void launch(Ball& b) {
        b.u = 1.0;
        b.s = 1.0;  // at the top of the arc: falling from here
        b.since = 0.0;
        b.flying = true;
    }

    void process(int frames) {
        const int n = max(1, channels());
        hit.setChannels(n);
        end.setChannels(n);
        height.setChannels(n);
        strength.setChannels(n);
        air.setChannels(n);
        if (!started) {
            started = true;
            if (loop.on())
                for (int c = 0; c < 16; ++c) launch(balls[c]);
        }
        const double dt = 1.0 / sampleRate();
        for (int i = 0; i < frames; ++i) {
            if (resetEdge.rose(reset[0][i])) {
                rng.seed(salt);
                for (int c = 0; c < 16; ++c) launch(balls[c]);
            }
            for (int c = 0; c < n; ++c) {
                Ball& b = balls[c];
                if (b.edge.rose(drop[c][i])) launch(b);
                const double t = max(0.001, fall.at(i, c) / 1000.0);
                if (b.flying) {
                    b.s += dt / t;
                    b.since += dt / t;
                    if (b.s >= 2.0 * b.u) {
                        // A hit: one draw, whatever Rough, so the floor
                        // stays the same floor.
                        const double r = rng.next() * 2.0 - 1.0;
                        const double keep = clamp(
                            bounce.at(i, c) * (1.0 + 0.5 * clamp(rough.at(i, c), 0.0, 1.0) * r),
                            0.0, 0.99);
                        b.last = b.u;
                        b.hitPulse.fire();
                        b.s -= 2.0 * b.u;
                        b.u *= keep;
                        // Settled once a flight would be shorter than 2 ms.
                        if (2.0 * b.u * t < 0.002) {
                            b.flying = false;
                            b.endPulse.fire();
                            if (loop.on()) launch(b);
                        }
                    }
                }
                const double h = b.flying ? clamp(2.0 * b.u * b.s - b.s * b.s, 0.0, 1.0) : 0.0;
                hit[c][i] = b.hitPulse.next();
                end[c][i] = b.endPulse.next();
                height[c][i] = h;
                strength[c][i] = b.last;
                air[c][i] = b.flying ? 1.0 : 0.0;
            }
        }
        shownHeight.set(static_cast<float>(height[0][frames - 1]));
        shownSince.set(static_cast<float>(balls[0].since));
        shownFlying.set(balls[0].flying ? 1 : 0);
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(160, 352);
        p.display(8, 28, 144, 110);
        p.knob(fall, 45, 178);
        p.knob(bounce, 115, 178);
        p.knob(rough, 45, 236);
        p.toggle(loop, 93, 228, 44, 16);
        p.input(drop, 30, 290);
        p.input(reset, 63, 290);
        p.output(hit, 97, 290);
        p.output(end, 130, 290);
        p.output(height, 47, 330);
        p.output(strength, 80, 330);
        p.output(air, 113, 330);
    }

    // The whole run of arcs at Bounce, left to right in time, and the ball
    // where it is now.
    void draw(Canvas& g) {
        g.well(8, 28, 144, 110);
        const double x0 = 14.0;
        const double w = 132.0;
        const double floorY = 130.0;
        const double top = 38.0;
        const double e = clamp(bounce.value(), 0.0, 0.99);
        // Time to settle, in falls: 1 + 2e / (1 - e), at most 40.
        const double total = min(40.0, 1.0 + 2.0 * e / max(0.01, 1.0 - e));
        const double xs = w / total;
        g.strokeStyle(Theme::line);
        g.lineWidth(1.0);
        g.beginPath();
        g.moveTo(x0, floorY + 1.0);
        g.lineTo(x0 + w, floorY + 1.0);
        g.stroke();
        g.strokeStyle(Theme::accent);
        g.lineWidth(1.4);
        g.beginPath();
        double at = 0.0;  // falls from the drop
        double u = 1.0;
        bool first = true;
        for (int k = 0; k < 40 && at < total; ++k) {
            const double from = first ? 1.0 : 0.0;
            const double to = 2.0 * u;
            for (int j = 0; j <= 16; ++j) {
                const double s = from + (to - from) * j / 16.0;
                const double h = clamp(2.0 * u * s - s * s, 0.0, 1.0);
                const double x = x0 + (at + s - from) * xs;
                const double y = floorY - h * (floorY - top);
                if (first && j == 0)
                    g.moveTo(x, y);
                else
                    g.lineTo(x, y);
            }
            at += to - from;
            first = false;
            u *= e;
            if (2.0 * u < 0.004) break;
        }
        g.stroke();
        if (shownFlying.get() == 1) {
            const double hNow = static_cast<double>(shownHeight.get());
            g.fillStyle(Theme::text);
            g.beginPath();
            const double x = x0 + clamp(static_cast<double>(shownSince.get()), 0.0, total) * xs;
            g.circle(x, floorY - 4.0 - hNow * (floorY - top - 4.0), 4.0);
            g.fill();
        }
    }
};
