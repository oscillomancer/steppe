// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Feel for gates, poly: each gate comes out where a player would put it,
// with the velocity a player would give it. Feel says which steps of the
// bar are strong: the downbeats, the backbeat (2 and 4), the offbeats (for
// hats) or the sixteenths pushed before each beat; Depth how much quieter
// the weak steps are than Level. Swing makes every second sixteenth (or
// eighth) late by a share of a step, Lay Back every gate late by
// milliseconds, behind the beat, and Human a little of both at random. A
// gate keeps its length; a gate with Accent high plays at Level. The steps
// are the song's sixteenths, or Clock's edges from Reset. Velocity holds
// each note's level from its gate's rise: patch it to a drum's Accent, a
// VCA, or rack.capture's velocity.

constexpr int kGrooveQueue = 8;

// How strong step s (a sixteenth of the bar, from 0) is in a feel, 0..1.
double grooveWeight(int feel, int step) {
    const int s = wrap(step, 16);
    const int q = imod(s, 4);
    switch (feel) {
        case 1:  // downbeats
            if (s == 0) return 1.0;
            if (s == 8) return 0.8;
            if (q == 0) return 0.6;
            return q == 2 ? 0.4 : 0.2;
        case 2:  // backbeat
            if (s == 4 || s == 12) return 1.0;
            if (q == 0) return 0.7;
            return q == 2 ? 0.4 : 0.2;
        case 3:  // offbeats
            if (q == 2) return 1.0;
            return q == 0 ? 0.5 : 0.25;
        case 4:  // pushed: the sixteenth before each beat
            if (q == 3) return 1.0;
            return q == 0 ? 0.6 : 0.3;
        default:
            return 1.0;
    }
}

// How many steps of Swing step s is moved: in sixteenths, every second
// one by one; in eighths the beat is bent, the offbeat eighth by two and
// the sixteenths either side of it by one, so they stay in order.
double grooveSwung(int on, int step) {
    const int q = imod(wrap(step, 16), 4);
    if (on == 0) return imod(q, 2) == 1 ? 1.0 : 0.0;
    if (q == 2) return 2.0;
    return q == 0 ? 0.0 : 1.0;
}

struct Groove : Module {
    static constexpr ModuleInfo info = {
        .name = "Groove",
        .id = "com.suppleaudio.rack.groove",
        .vendor = "Supple Audio",
        .description = "Feel for gates: accents by step, swing, lay-back and a human hand.",
        .tags = {"clock", "generative", "random", "poly"}};

    Param feel = choice("Feel", {"Even", "Downbeats", "Backbeat", "Offbeats", "Pushed"}, 1)
                     .keys({"even", "downbeats", "backbeat", "offbeats", "pushed"})
                     .help(
                         "Which steps of the bar are strong: none, the downbeats, 2 and 4, "
                         "the offbeats, or the sixteenth before each beat.");
    Param level = percent("Level", 85).help("The velocity of the strongest steps.");
    Param depth =
        percent("Depth", 50)
            .help(
                "How much quieter the weakest steps are: 0 % all at Level, 100 % down to nothing.");
    Param swing = percent("Swing", 0, 0, 75)
                      .help(
                          "How late every second step comes, as a share of a step: about 33 % is a "
                          "triplet feel.");
    Param swing_on = choice("Swing On", {"16ths", "8ths"}, 0)
                         .keys({"sixteenths", "eighths"})
                         .help(
                             "What Swing bends: every second sixteenth, or every second eighth "
                             "(with the sixteenths around it).");
    Param lay_back =
        milliseconds("Lay Back", 0, 40, 0).help("How late every gate comes: behind the beat.");
    Param human =
        percent("Human", 0)
            .help("A player's unevenness: up to 12 ms late and 12 % louder or softer, at random.");

    Input gate = gateIn("Gate").help("The gates to play with feel (rise 0.5, fall 0.25); poly.");
    Input accent = gateIn("Accent").help("High as a gate rises: it plays at Level.");
    Input clock = gateIn("Clock").help(
        "Each rising edge is the next step; with nothing here, the song's sixteenths.");
    Input reset = gateIn("Reset").help("A rising edge makes the next step the bar's first.");

    Output out = gateOut("Gate").help("The gates, moved by Swing, Lay Back and Human.");
    Output velocity =
        unipolarOut("Velocity").help("Each note's level, held from its gate's rise; poly.");
    Output weight = unipolarOut("Weight").help("How strong the step playing is, 0 to 1.");

    struct Voice {
        bool in = false;   // the input gate
        bool on = false;   // the output gate
        int delay = 0;     // frames the last rise was moved
        double vel = 0.0;  // the last rise's velocity
        int head = 0;
        int count = 0;
        Array<int, kGrooveQueue> due;      // frame each event is due
        Array<double, kGrooveQueue> rise;  // 1 a rise, 0 a fall
        Array<double, kGrooveQueue> loud;  // a rise's velocity
    };
    Array<Voice, 16> voices;
    Random rng;
    Trigger clockEdge;
    Trigger resetEdge;
    int salt = 0;
    int now = 0;       // frames since the start
    int steps = -1;    // Clock's edges since Reset
    int since = 0;     // frames since Clock's last edge
    int period = 0;    // frames between its last two
    double shift = 0;  // sixteenths added to the song's, by Reset

    Shared<int> shownStep;

    void factoryPatches(Patches& p) {
        p.add("Downbeats");
        p.add("Backbeat Swing").set(feel, "backbeat").set(swing, 30);
        p.add("Offbeat Hats").set(feel, "offbeats").set(depth, 70).set(human, 30);
        p.add("Lazy").set(feel, "downbeats").set(lay_back, 18).set(human, 40);
        p.add("Shuffle").set(feel, "even").set(swing, 50).set(swing_on, "eighths");
    }

    void prepare() {
        salt = seed();
        rng.seed(salt);
    }

    void push(Voice& v, int at, double r, double vel) {
        if (v.count == kGrooveQueue) {
            v.head = wrap(v.head + 1, kGrooveQueue);
            --v.count;
        }
        // Events stay in order: a rise moved less than the last fall comes after it.
        if (v.count > 0) {
            const int last = v.due[wrap(v.head + v.count - 1, kGrooveQueue)];
            if (at <= last) at = last + 1;
        }
        const int k = wrap(v.head + v.count, kGrooveQueue);
        v.due[k] = at;
        v.rise[k] = r;
        v.loud[k] = vel;
        ++v.count;
    }

    void process(int frames) {
        const int n = max(1, gate.channels());
        out.setChannels(n);
        velocity.setChannels(n);
        weight.setChannels(1);
        const Transport t = transport();
        const bool internal = !clock.connected();
        const double rate = sampleRate();
        const double beatsPerFrame = t.tempo / 60.0 / rate;
        const int f = feel.index();
        int playing = 0;
        for (int i = 0; i < frames; ++i) {
            const double song = (t.beats + i * beatsPerFrame) * 4.0;  // sixteenths
            if (!internal) {
                ++since;
                if (clockEdge.rose(clock[0][i])) {
                    if (steps >= 0) period = since;
                    since = 0;
                    ++steps;
                }
            }
            if (resetEdge.rose(reset[0][i])) {
                rng.seed(salt);
                if (internal)
                    shift = -floor(song + 0.5);
                else
                    steps = -1;
            }
            // The step playing, and the frames a step lasts.
            int step = 0;
            double stepFrames = rate * 15.0 / max(1.0, t.tempo);
            if (internal) {
                step = roundToInt(song + shift);
            } else {
                if (period > 0) stepFrames = period;
                step = max(0, steps);
                if (steps >= 0 && since > stepFrames / 2) ++step;  // just before the next edge
            }
            const double w = grooveWeight(f, step);
            weight[0][i] = w;
            for (int c = 0; c < n; ++c) {
                Voice& v = voices[c];
                const double g = gate[c][i];
                if (!v.in && g >= 0.5) {
                    v.in = true;
                    // Two draws a rise, whatever the settings.
                    const double late = rng.next();
                    const double louder = rng.next() * 2.0 - 1.0;
                    const double h = human.at(i, c);
                    double d = lay_back.at(i, c) * 0.001 * rate + h * late * 0.012 * rate;
                    d += swing.at(i, c) * grooveSwung(swing_on.index(), step) * stepFrames;
                    v.delay = max(0, roundToInt(d));
                    const double strong = accent[c][i] >= 0.5 ? 1.0 : w;
                    const double vel = level.at(i, c) * (1.0 - depth.at(i, c) * (1.0 - strong));
                    push(v, now + v.delay, 1.0, clamp(vel + 0.12 * h * louder, 0.01, 1.0));
                } else if (v.in && g < 0.25) {
                    v.in = false;
                    push(v, now + v.delay, 0.0, 0.0);
                }
                // What is due now; a rise on a high gate first drops it for a frame.
                if (v.count > 0 && v.due[v.head] <= now) {
                    if (v.rise[v.head] > 0.5 && v.on) {
                        v.on = false;
                        v.due[v.head] = now + 1;
                    } else {
                        v.on = v.rise[v.head] > 0.5;
                        if (v.on) v.vel = v.loud[v.head];
                        v.head = wrap(v.head + 1, kGrooveQueue);
                        --v.count;
                    }
                }
                out[c][i] = v.on ? 1.0 : 0.0;
                velocity[c][i] = v.vel;
            }
            ++now;
            playing = wrap(step, 16);
        }
        shownStep.set(playing);
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(176, 372);
        p.display(8, 28, 160, 62);
        p.choice(feel, 8, 98, 160, 18);
        p.knob(level, 30, 148);
        p.knob(depth, 88, 148);
        p.knob(human, 146, 148);
        p.knob(swing, 30, 206);
        p.knob(lay_back, 88, 206);
        p.choice(swing_on, 8, 244, 160, 18);
        p.input(gate, 30, 292);
        p.input(accent, 74, 292);
        p.input(clock, 118, 292);
        p.input(reset, 156, 292);
        p.output(out, 52, 340);
        p.output(velocity, 100, 340);
        p.output(weight, 148, 340);
    }

    // The bar's sixteen steps as bars as tall as their velocity, the swung
    // ones drawn late; the step playing bright.
    void draw(Canvas& g) {
        const double left = 8.0;
        const double top = 28.0;
        const double w = 160.0;
        const double h = 62.0;
        g.well(left, top, w, h);
        const double x0 = left + 6.0;
        const double cell = (w - 12.0) / 16.0;
        const double base = top + h - 8.0;
        const double tall = h - 16.0;
        const int f = feel.index();
        const int lit = shownStep.get();
        const double sw = clamp(swing.value(), 0.0, 0.75);
        const double lv = clamp(level.value(), 0.0, 1.0);
        const double dp = clamp(depth.value(), 0.0, 1.0);
        // The beats.
        g.strokeStyle(Theme::line);
        g.lineWidth(1.0);
        g.beginPath();
        for (int b = 1; b < 4; ++b) {
            const double x = x0 + cell * 4.0 * b;
            g.moveTo(x, top + 6.0);
            g.lineTo(x, base + 3.0);
        }
        g.stroke();
        for (int s = 0; s < 16; ++s) {
            const double late = sw * cell * grooveSwung(swing_on.index(), s);
            const double vel = max(0.04, lv * (1.0 - dp * (1.0 - grooveWeight(f, s))));
            g.fillStyle(s == lit ? Theme::text : Theme::accent);
            g.globalAlpha(s == lit ? 1.0 : 0.8);
            g.beginPath();
            g.roundRect(x0 + cell * s + late + 1.0, base - tall * vel, cell * 0.5, tall * vel, 1.5);
            g.fill();
        }
        g.globalAlpha(1.0);
    }
};
