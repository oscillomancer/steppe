// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A strange attractor: a point that loops forever around a shape it never
// quite repeats, its three coordinates out as slow, organic CVs. Lorenz is
// the butterfly, two wings it flips between at no fixed time; Thomas is a
// smoother, rounder knot that wanders through all three axes alike. Rate
// is about how many loops a second; Chaos pushes the system from its
// calmer edge into wilder motion. At Voices above 1 each channel runs its
// own copy, started a hair apart: they move together at first, then drift
// into motions of their own. Reset starts them all over from the same
// place, so a patch's motion is the same on every play.

constexpr int kTrail = 256;

struct Chaos : Module {
    static constexpr ModuleInfo info = {
        .name = "Chaos",
        .id = "com.suppleaudio.rack.chaos",
        .vendor = "Supple Audio",
        .description = "A strange attractor (Lorenz or Thomas): X, Y and Z as slow organic CVs.",
        .tags = {"modulation", "random", "generative", "poly"}};

    Param attractor = choice("Attractor", {"Lorenz", "Thomas"}, 0)
                          .help("Lorenz flips between two wings; Thomas wanders a rounder knot.");
    Param rate = frequency("Rate", 0.01, 20, 0.3).help("About how many loops it makes a second.");
    Param chaos =
        percent("Chaos", 50)
            .help("From the calmer edge of the system (0 %) to its wildest motion (100 %).");
    Param voices =
        integer("Voices", 1, 16, 1)
            .help("How many copies run, one a channel, started a hair apart so they drift apart.");

    Input reset = gateIn("Reset").help("A rising edge starts every copy over from its start.");

    Output x = cvOut("X").help("The first coordinate, -1 to 1.");
    Output y = cvOut("Y").help("The second coordinate, -1 to 1.");
    Output z = cvOut("Z").help("The third coordinate, -1 to 1.");

    struct Point {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };
    Array<Point, 16> state;
    bool resetHigh = false;
    double nudge = 0.0;  // from the seed: where the first copy starts
    int kind = -1;       // the attractor the state belongs to
    int sinceTrail = 0;

    SharedArray<float, 256> trailX;
    SharedArray<float, 256> trailY;
    Shared<int> shownKind;

    void factoryPatches(Patches& p) {
        p.add("Butterfly").set(attractor, "lorenz").set(rate, 0.3);
        p.add("Slow Knot").set(attractor, "thomas").set(rate, 0.05).set(chaos, 70);
        p.add("Drifting Quartet").set(attractor, "thomas").set(voices, 4).set(rate, 0.1);
        p.add("Nervous").set(attractor, "lorenz").set(rate, 4).set(chaos, 100);
    }

    void startOver() {
        for (int c = 0; c < 16; ++c) {
            Point& p = state[c];
            const double d = nudge + 0.001 * c;
            if (kind == 1) {
                p.x = 1.0 + d;
                p.y = -0.5;
                p.z = 0.3;
            } else {
                p.x = 1.0 + d;
                p.y = 1.0;
                p.z = 20.0;
            }
        }
    }

    void prepare() {
        Random r;
        r.seed(seed());
        nudge = 0.01 * r.next();
        kind = clamp(attractor.index(), 0, 1);
        startOver();
        for (int k = 0; k < kTrail; ++k) {
            trailX[k] = 0.0f;
            trailY[k] = 0.0f;
        }
    }

    // The flow at p: Lorenz with rho from the chaos, or Thomas with its
    // damping from the chaos.
    void flow(const Point& p, double amount, Point& d) const {
        if (kind == 1) {
            const double b = 0.21 - 0.12 * amount;
            d.x = sin(p.y) - b * p.x;
            d.y = sin(p.z) - b * p.y;
            d.z = sin(p.x) - b * p.z;
        } else {
            const double rho = 25.0 + 30.0 * amount;
            d.x = 10.0 * (p.y - p.x);
            d.y = p.x * (rho - p.z) - p.y;
            d.z = p.x * p.y - 8.0 / 3.0 * p.z;
        }
    }

    void moved(const Point& p, const Point& d, double h, Point& q) const {
        q.x = p.x + d.x * h;
        q.y = p.y + d.y * h;
        q.z = p.z + d.z * h;
    }

    // One Runge-Kutta step of h.
    void advance(Point& p, double h, double amount) {
        Point k1;
        Point k2;
        Point k3;
        Point k4;
        Point q;
        flow(p, amount, k1);
        moved(p, k1, 0.5 * h, q);
        flow(q, amount, k2);
        moved(p, k2, 0.5 * h, q);
        flow(q, amount, k3);
        moved(p, k3, h, q);
        flow(q, amount, k4);
        p.x += h / 6.0 * (k1.x + 2.0 * k2.x + 2.0 * k3.x + k4.x);
        p.y += h / 6.0 * (k1.y + 2.0 * k2.y + 2.0 * k3.y + k4.y);
        p.z += h / 6.0 * (k1.z + 2.0 * k2.z + 2.0 * k3.z + k4.z);
        if (!(isFinite(p.x) && isFinite(p.y) && isFinite(p.z))) {
            p.x = 1.0;
            p.y = 1.0;
            p.z = kind == 1 ? 0.3 : 20.0;
        }
    }

    void process(int frames) {
        const int n = max(channels(), voices.index());
        x.setChannels(n);
        y.setChannels(n);
        z.setChannels(n);
        const int k = clamp(attractor.index(), 0, 1);
        if (k != kind) {
            kind = k;
            startOver();
        }
        // Time units a loop takes, roughly: Lorenz's are short, Thomas's long.
        const double loop = kind == 1 ? 9.0 : 0.75;
        for (int i = 0; i < frames; ++i) {
            const double r = reset[0][i];
            if (!resetHigh && r >= 0.5) {
                resetHigh = true;
                startOver();
            } else if (resetHigh && r < 0.25) {
                resetHigh = false;
            }
            for (int c = 0; c < n; ++c) {
                Point& p = state[c];
                const double h = clamp(rate.at(i, c), 0.0, 20.0) * loop / sampleRate();
                advance(p, h, clamp(chaos.at(i, c), 0.0, 1.0));
                if (kind == 1) {
                    x[c][i] = tanh(p.x / 4.0);
                    y[c][i] = tanh(p.y / 4.0);
                    z[c][i] = tanh(p.z / 4.0);
                } else {
                    x[c][i] = tanh(p.x / 20.0);
                    y[c][i] = tanh(p.y / 27.0);
                    z[c][i] = tanh((p.z - 25.0) / 20.0);
                }
            }
        }
        // The first copy's path, 32 points a loop: about eight loops shown.
        sinceTrail += frames;
        if (sinceTrail >= roundToInt(sampleRate() / max(0.01, rate.value()) / 32.0)) {
            sinceTrail = 0;
            for (int q = 0; q < kTrail - 1; ++q) {
                trailX[q] = trailX[q + 1];
                trailY[q] = trailY[q + 1];
            }
            trailX[kTrail - 1] = static_cast<float>(x[0][frames - 1]);
            trailY[kTrail - 1] =
                static_cast<float>(kind == 1 ? y[0][frames - 1] : z[0][frames - 1]);
        }
        shownKind.set(kind);
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(176, 284);
        p.display(8, 28, 160, 110);
        p.choice(attractor, 8, 146, 160, 18);
        p.knob(rate, 30, 194);
        p.knob(chaos, 88, 194);
        p.knob(voices, 146, 194);
        p.input(reset, 22, 256);
        p.output(x, 88, 256);
        p.output(y, 122, 256);
        p.output(z, 156, 256);
    }

    void draw(Canvas& g) {
        g.well(8, 28, 160, 110);
        const double cx = 88.0;
        const double cy = 83.0;
        const double rx = 72.0;
        const double ry = 48.0;
        g.lineWidth(1.3);
        g.lineCap(LineCap::Round);
        // The trail, fading towards its oldest end.
        for (int k = 1; k < kTrail; ++k) {
            g.globalAlpha(0.08 + 0.92 * k / kTrail);
            g.strokeStyle(Theme::accent);
            g.beginPath();
            g.moveTo(cx + rx * static_cast<double>(trailX[k - 1]),
                     cy - ry * static_cast<double>(trailY[k - 1]));
            g.lineTo(cx + rx * static_cast<double>(trailX[k]),
                     cy - ry * static_cast<double>(trailY[k]));
            g.stroke();
        }
        g.globalAlpha(1.0);
        g.fillStyle(Theme::text);
        g.beginPath();
        g.circle(cx + rx * static_cast<double>(trailX[kTrail - 1]),
                 cy - ry * static_cast<double>(trailY[kTrail - 1]), 2.6);
        g.fill();
    }
};
