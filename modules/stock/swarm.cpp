// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// LFOs that listen to each other, like fireflies flashing in a tree. Each
// voice runs at its own rate, spread around Rate, and is pulled towards the
// swarm's average phase by Coupling: weak, they drift apart and the sum
// churns; past a point they fall into step one by one and lock, the sum
// swelling to a single wave. Negative Coupling pushes them apart instead,
// until they share the cycle evenly. Noise jostles the phases, so the lock
// breaks and forms again. Out sends each voice's sine (a poly cable: a
// voice a filter, a pan, a level), Phase its ramp, Trigger a pulse as each
// begins a cycle; Mean is the voices' average, Sync how together they are,
// 0 scattered to 1 in step. Scatter throws every phase anew; Reset starts
// the same swarm over.

constexpr int kSwarmMax = 8;

struct Swarm : Module {
    static constexpr ModuleInfo info = {
        .name = "Swarm",
        .id = "com.suppleaudio.rack.swarm",
        .vendor = "Supple Audio",
        .description = "Coupled LFOs that pull into step or push apart, like fireflies.",
        .tags = {"modulation", "lfo", "generative", "poly"}};

    Param voices = integer("Voices", 2, kSwarmMax, 4).help("How many oscillators, a channel each.");
    Param rate =
        frequency("Rate", 0.01, 20, 0.5).help("The voices' middle rate, in cycles a second.");
    Param spread =
        percent("Spread", 30)
            .help(
                "How far the voices' own rates differ: up to half an octave either side at 100 %.");
    Param coupling = percent("Coupling", 30, -100, 100)
                         .help(
                             "How hard each voice is pulled towards the others' phase; below 0, "
                             "pushed away until they share the cycle evenly.");
    Param noise =
        percent("Noise", 0).help("Random nudges to every phase, so the lock breaks and forms.");

    Input reset = gateIn("Reset").help("A rising edge starts the same swarm over.");
    Input scatter = gateIn("Scatter").help("A rising edge throws every phase somewhere new.");

    Output out = cvOut("Out").help("Each voice's sine, -1 to 1, a channel each.");
    Output phase = phaseOut("Phase").help("Each voice's place in its cycle, 0 to 1.");
    Output trigger = gateOut("Trigger").help("A trigger as each voice begins a cycle.");
    Output mean =
        cvOut("Mean").help("The voices' sines averaged: still when apart, a full wave in step.");
    Output sync = unipolarOut("Sync").help("How together the phases are: 0 scattered, 1 in step.");

    Array<double, kSwarmMax> at;  // each voice's phase, 0..1
    Array<Pulse, kSwarmMax> pulses;
    Random rng;
    Trigger resetEdge;
    Trigger scatterEdge;
    int salt = 0;
    bool started = false;
    double order = 0.0;
    double centre = 0.0;  // the swarm's mean phase, 0..1

    SharedArray<float, kSwarmMax> shownAt;
    Shared<int> shownCount;
    Shared<float> shownOrder;
    Shared<float> shownCentre;

    void factoryPatches(Patches& p) {
        p.add("Fireflies").set(voices, 8).set(spread, 40).set(coupling, 25).set(noise, 10);
        p.add("Lock Step").set(coupling, 80);
        p.add("Splay").set(coupling, -60).set(spread, 10);
        p.add("Drift Apart").set(coupling, 0).set(spread, 60).set(rate, 0.2);
        p.add("Restless").set(voices, 6).set(coupling, 45).set(noise, 50);
    }

    void prepare() {
        salt = seed();
        started = false;  // sown on the first block
    }

    void throwPhases() {
        // Eight draws whatever Voices, so a swarm depends only on its seed.
        for (int k = 0; k < kSwarmMax; ++k) at[k] = rng.next();
    }

    void startOver() {
        rng.seed(salt);
        throwPhases();
    }

    // Voice k's own rate, as a share of Rate: evenly from the slowest to the
    // fastest, the middle at Rate.
    double ownRate(int k, int n, double share) const {
        const double d = n > 1 ? 2.0 * k / (n - 1) - 1.0 : 0.0;
        return pow(2.0, 0.5 * share * d);
    }

    void process(int frames) {
        if (!started) {
            started = true;
            startOver();
        }
        const int n = clamp(voices.index(), 2, kSwarmMax);
        out.setChannels(n);
        phase.setChannels(n);
        trigger.setChannels(n);
        mean.setChannels(1);
        sync.setChannels(1);
        const double dt = 1.0 / sampleRate();
        const double jolt = sqrt(12.0 * dt);  // a uniform step with a variance of dt
        for (int i = 0; i < frames; ++i) {
            if (resetEdge.rose(reset[0][i])) startOver();
            if (scatterEdge.rose(scatter[0][i])) throwPhases();
            const double f = clamp(rate.at(i), 0.01, 20.0);
            const double share = clamp(spread.at(i), 0.0, 1.0);
            const double pull = 2.0 * f * clamp(coupling.at(i), -1.0, 1.0);
            const double shake = 0.5 * f * clamp(noise.at(i), 0.0, 1.0) * jolt;
            // The mean field: every voice feels the swarm's average.
            double c = 0.0;
            double s = 0.0;
            for (int k = 0; k < n; ++k) {
                c += cos(2.0 * pi * at[k]);
                s += sin(2.0 * pi * at[k]);
            }
            c /= n;
            s /= n;
            order = sqrt(c * c + s * s);
            centre = atan2(s, c) / (2.0 * pi);
            if (centre < 0.0) centre += 1.0;
            double sum = 0.0;
            for (int k = 0; k < kSwarmMax; ++k) {
                // A draw a voice a frame, whatever Noise and Voices.
                const double u = rng.next() - 0.5;
                if (k >= n) continue;
                const double a = 2.0 * pi * at[k];
                const double speed = f * ownRate(k, n, share) + pull * (s * cos(a) - c * sin(a));
                double next = at[k] + speed * dt + shake * u;
                if (next >= 1.0) {
                    next -= floor(next);
                    pulses[k].fire();
                } else if (next < 0.0) {
                    next -= floor(next);
                }
                at[k] = next;
                const double v = sin(2.0 * pi * next);
                out[k][i] = v;
                phase[k][i] = next;
                trigger[k][i] = pulses[k].next();
                sum += v;
            }
            mean[0][i] = sum / n;
            sync[0][i] = order;
        }
        for (int k = 0; k < kSwarmMax; ++k) shownAt[k] = static_cast<float>(at[k]);
        shownCount.set(n);
        shownOrder.set(static_cast<float>(order));
        shownCentre.set(static_cast<float>(centre));
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(180, 384);
        p.display(8, 28, 164, 130);
        p.knob(rate, 50, 200);
        p.knob(coupling, 130, 200);
        p.knob(spread, 32, 262, 28);
        p.knob(noise, 90, 262, 28);
        p.knob(voices, 148, 262, 28);
        p.input(reset, 32, 318);
        p.input(scatter, 70, 318);
        p.output(out, 110, 318);
        p.output(phase, 148, 318);
        p.output(trigger, 51, 362);
        p.output(mean, 90, 362);
        p.output(sync, 129, 362);
    }

    // The voices as fireflies round a ring, and an arrow from the middle for
    // the swarm: long when they are in step, short when scattered.
    void draw(Canvas& g) {
        g.well(8, 28, 164, 130);
        const double cx = 90.0;
        const double cy = 93.0;
        const double r = 50.0;
        g.strokeStyle(Theme::line);
        g.lineWidth(1.0);
        g.beginPath();
        g.circle(cx, cy, r);
        g.stroke();
        const int n = shownCount.get();
        for (int k = 0; k < n; ++k) {
            const double a = 2.0 * pi * static_cast<double>(shownAt[k]) - pi / 2.0;
            g.fillStyle(imod(k, 2) == 0 ? Theme::accent : Theme::accent2);
            g.beginPath();
            g.circle(cx + r * cos(a), cy + r * sin(a), 4.0);
            g.fill();
        }
        const double len = r * static_cast<double>(shownOrder.get());
        const double a = 2.0 * pi * static_cast<double>(shownCentre.get()) - pi / 2.0;
        g.strokeStyle(Theme::text);
        g.lineWidth(1.6);
        g.beginPath();
        g.moveTo(cx, cy);
        g.lineTo(cx + len * cos(a), cy + len * sin(a));
        g.stroke();
        g.fillStyle(Theme::text);
        g.beginPath();
        g.circle(cx, cy, 2.0);
        g.fill();
    }
};
