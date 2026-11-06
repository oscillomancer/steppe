// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Random events with no clock: drops on a roof, crackle, a rain of plucks.
// Density is how many come a second on average, at random moments; Cluster
// makes each event briefly raise the odds of another, so they come in
// bursts and flurries with quiet between (at 0 they are evenly random).
// Grid holds each event to the song's next eighth, sixteenth or
// thirty-second, so the chance falls in time (free while the song is
// stopped). Trigger fires at each event; Velocity is its level, varied by
// Vary; Value a random -1 to 1 held until the next, for a pitch or a pan.
// Voices sends that many independent streams on a poly cable. Reset starts
// the same rain over.

constexpr int kDustShown = 32;

struct Dust : Module {
    static constexpr ModuleInfo info = {
        .name = "Dust",
        .id = "com.suppleaudio.rack.dust",
        .vendor = "Supple Audio",
        .description = "Random triggers with no clock, evenly scattered or in bursts.",
        .tags = {"generative", "random", "rhythm", "poly"}};

    Param density = frequency("Density", 0.1, 50, 4).help("How many events a second on average.");
    Param cluster = percent("Cluster", 0)
                        .help(
                            "How much each event raises the odds of another soon after: bursts "
                            "with quiet between.");
    Param vary = percent("Vary", 50).help("How much each event's Velocity varies below 1.");
    Param voices = integer("Voices", 1, 8, 1).help("How many independent streams, a channel each.");
    Param grid =
        choice("Grid", {"Free", "1/8", "1/16", "1/32"}, 0)
            .keys({"free", "n8", "n16", "n32"})
            .help("Hold each event to the song's next note of this length; free while stopped.");

    Input reset = gateIn("Reset").help("A rising edge starts the same events over.");

    Output trigger = gateOut("Trigger").help("A trigger at each event.");
    Output velocity = unipolarOut("Velocity").help("The last event's level, held: 1 at most.");
    Output value = cvOut("Value").help("A random value from -1 to 1 for each event, held.");

    struct Stream {
        Pulse pulse;
        double excite = 0.0;  // the extra rate the last events left, a second
        double level = 0.0;
        double held = 0.0;
        bool pending = false;  // waiting for the grid
        double pendingLevel = 0.0;
        double pendingHeld = 0.0;
        int cell = -1;
        int quiet = 0;  // frames left of the last trigger
    };
    Array<Stream, 8> streams;
    Random rng;
    Trigger resetEdge;
    int salt = 0;
    bool started = false;
    double now = 0.0;  // seconds since the start, for the face
    Array<double, kDustShown> when;
    Array<double, kDustShown> how;
    int written = 0;

    SharedArray<float, kDustShown> shownAgo;
    SharedArray<float, kDustShown> shownLevel;

    void factoryPatches(Patches& p) {
        p.add("Light Rain").set(density, 6).set(vary, 60);
        p.add("Crackle").set(density, 30).set(cluster, 60).set(vary, 80);
        p.add("Flurries").set(density, 1.5).set(cluster, 90);
        p.add("Sixteenth Chance").set(density, 3).set(grid, "n16").set(vary, 40);
        p.add("Four Drops").set(voices, 4).set(density, 2);
    }

    void prepare() {
        salt = seed();
        started = false;  // started on the first block
    }

    void startOver() {
        rng.seed(salt);
        for (int c = 0; c < 8; ++c) {
            Stream& s = streams[c];
            s.excite = 0.0;
            s.level = 0.0;
            s.held = 0.0;
            s.pending = false;
            s.cell = -1;
            s.quiet = 0;
        }
        for (int k = 0; k < kDustShown; ++k) {
            when[k] = -100.0;
            how[k] = 0.0;
        }
        written = 0;
    }

    double perBeat() const {
        switch (grid.index()) {
            case 1:
                return 2.0;
            case 2:
                return 4.0;
            case 3:
                return 8.0;
            default:
                return 0.0;
        }
    }

    void fire(Stream& s, int c, double level, double held) {
        // One event a trigger: one that comes while the last trigger is
        // still high is left out, so Value never moves without a trigger.
        if (s.quiet > 0) return;
        s.quiet = max(1, roundToInt(0.001 * sampleRate()));
        s.pulse.fire();
        s.level = level;
        s.held = held;
        if (c == 0) {
            when[written] = now;
            how[written] = level;
            written = written + 1 >= kDustShown ? 0 : written + 1;
        }
    }

    void process(int frames) {
        if (!started) {
            started = true;
            startOver();
        }
        const int n = clamp(voices.index(), 1, 8);
        trigger.setChannels(n);
        velocity.setChannels(n);
        value.setChannels(n);
        const Transport t = transport();
        const double dt = 1.0 / sampleRate();
        const double beatsPerFrame = t.tempo / 60.0 / sampleRate();
        const double cells = t.playing ? perBeat() : 0.0;
        // An event's burst dies away over 120 ms; at full Cluster it brings
        // nine tenths of an event more on average, so the flurries end.
        const double tau = 0.12;
        const double fade = exp(-dt / tau);
        for (int i = 0; i < frames; ++i) {
            if (resetEdge.rose(reset[0][i])) startOver();
            now += dt;
            const double base = clamp(density.at(i), 0.1, 50.0);
            const double jump = 0.9 * clamp(cluster.at(i), 0.0, 1.0) / tau;
            const double spread = clamp(vary.at(i), 0.0, 1.0);
            const int cell =
                cells > 0.0 ? floorToInt((t.beats + i * beatsPerFrame) * cells + 1e-9) : -1;
            // All eight streams run whatever Voices, so each one's events stay
            // the same as streams are added.
            for (int c = 0; c < 8; ++c) {
                Stream& s = streams[c];
                // One draw a stream a frame, and two more an event.
                const double u = rng.next();
                if (u < (base + s.excite) * dt) {
                    const double level = 1.0 - spread * rng.next();
                    const double held = rng.next() * 2.0 - 1.0;
                    s.excite += jump;
                    if (cell >= 0) {
                        s.pending = true;
                        s.pendingLevel = level;
                        s.pendingHeld = held;
                    } else {
                        fire(s, c, level, held);
                    }
                }
                s.excite *= fade;
                if (cell >= 0 && cell != s.cell) {
                    if (s.pending && s.cell >= 0) fire(s, c, s.pendingLevel, s.pendingHeld);
                    s.pending = false;
                }
                if (cell < 0 && s.pending) {
                    fire(s, c, s.pendingLevel, s.pendingHeld);  // the song stopped
                    s.pending = false;
                }
                s.cell = cell;
                if (s.quiet > 0) --s.quiet;
                const double pulse = s.pulse.next();
                if (c >= n) continue;
                trigger[c][i] = pulse;
                velocity[c][i] = s.level;
                value[c][i] = s.held;
            }
        }
        for (int k = 0; k < kDustShown; ++k) {
            shownAgo[k] = static_cast<float>(now - when[k]);
            shownLevel[k] = static_cast<float>(how[k]);
        }
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(160, 338);
        p.display(8, 28, 144, 100);
        p.knob(density, 45, 170);
        p.knob(cluster, 115, 170);
        p.knob(vary, 45, 226, 28);
        p.knob(voices, 115, 226, 28);
        p.choice(grid, 8, 256, 144, 18);
        p.input(reset, 30, 310);
        p.output(trigger, 63, 310);
        p.output(velocity, 97, 310);
        p.output(value, 130, 310);
    }

    // The last two seconds of the first stream, newest on the right: a
    // stroke an event, as tall as its Velocity.
    void draw(Canvas& g) {
        g.well(8, 28, 144, 100);
        const double x0 = 14.0;
        const double w = 132.0;
        const double floorY = 120.0;
        g.strokeStyle(Theme::line);
        g.lineWidth(1.0);
        g.beginPath();
        g.moveTo(x0, floorY + 0.5);
        g.lineTo(x0 + w, floorY + 0.5);
        g.stroke();
        g.strokeStyle(Theme::accent);
        g.lineWidth(2.0);
        for (int k = 0; k < kDustShown; ++k) {
            const double ago = static_cast<double>(shownAgo[k]);
            if (ago < 0.0 || ago > 2.0) continue;
            const double x = x0 + w * (1.0 - ago / 2.0);
            const double h = 80.0 * static_cast<double>(shownLevel[k]);
            g.globalAlpha(1.0 - 0.6 * ago / 2.0);
            g.beginPath();
            g.moveTo(x, floorY);
            g.lineTo(x, floorY - h);
            g.stroke();
        }
        g.globalAlpha(1.0);
    }
};
