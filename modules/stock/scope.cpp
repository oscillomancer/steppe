// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// An oscilloscope: two signals drawn as they move, A in the accent colour
// and B in the second. Time is how long the screen is, left to right. A
// sweep starts when the trigger source (A, B or the Trigger jack) crosses
// Level, rising or falling, so a steady wave stands still; with no
// crossing for a while it sweeps anyway (AUTO), and Free never waits.
// Synced, the screen spans a share of the song (a 1/1 is a 4/4 bar) and
// starts on it: an LFO or a sequence lines up under the beat. Range and
// Offset zoom and move each trace; Stacked gives each its own half, and
// X-Y draws A across against B up, a trail as long as Time (Lissajous
// figures, chaos's knots, a stereo field). Persistence leaves old sweeps
// glowing behind the new one. Each trace shows its cable's first channel.
// Sweep sends a trigger as each sweep starts.

constexpr int kPoints = 192;  // columns across the screen
constexpr int kLanes = 4;     // A's lows and highs, then B's
constexpr int kPast = 4;      // finished sweeps kept for the afterglow
constexpr int kTrail = 512;   // X-Y points
constexpr double kLeft = 8.0;
constexpr double kTop = 28.0;
constexpr double kWide = 284.0;
constexpr double kHigh = 184.0;

struct Scope : Module {
    static constexpr ModuleInfo info = {
        .name = "Scope",
        .id = "com.suppleaudio.rack.scope",
        .vendor = "Supple Audio",
        .description = "An oscilloscope: two traces, triggered or locked to the song, and X-Y.",
        .tags = {"utility", "scope", "display"}};

    Param view = choice("View", {"Overlaid", "Stacked", "X-Y"}, 0)
                     .keys({"overlaid", "stacked", "xy"})
                     .help(
                         "Both traces over the whole screen, each in its own half, or A "
                         "across against B up.");
    Param timeBase =
        milliseconds("Time", 1, 10000, 20)
            .help("How long the screen is, left to right; in X-Y, the trail's length.");
    Param sync =
        toggle("Sync", false)
            .help("While the song plays, each sweep spans Span of the song and starts on it.");
    Param span = choice("Span", {"1/16", "1/8", "1/4", "1/2", "1/1", "2/1", "4/1"}, 4)
                     .keys({"d1_16", "d1_8", "d1_4", "d1_2", "d1_1", "d2_1", "d4_1"})
                     .activeWhen(sync)
                     .help("How much of the song the screen shows, synced; 1/1 is a 4/4 bar.");
    Param source = choice("Trigger", {"Free", "A", "B", "Trigger In"}, 1)
                       .keys({"free", "a", "b", "trigger_in"})
                       .help(
                           "What starts a sweep: nothing (it runs free), A or B crossing "
                           "Level, or a rising edge at Trigger.");
    Param level = continuous("Level", -2, 2, 0)
                      .activeWhen(source, {"a", "b"})
                      .help("The value the source crosses to start a sweep (the left marker).");
    Param slope = choice("Slope", {"Rising", "Falling"}, 0)
                      .activeWhen(source, {"a", "b"})
                      .help("Whether the source crosses Level going up or going down.");
    Param persistence = percent("Glow", 40).help("How long old sweeps glow behind the new one.");
    Param rangeA = continuous("Range A", 0.01, 10, 1)
                       .curved(Curve::Log)
                       .help("The value at the top of A's trace: smaller zooms in.");
    Param offsetA = continuous("Offset A", -1, 1, 0).help("Moves A up or down, in half screens.");
    Param rangeB = continuous("Range B", 0.01, 10, 1)
                       .curved(Curve::Log)
                       .help("The value at the top of B's trace: smaller zooms in.");
    Param offsetB = continuous("Offset B", -1, 1, 0).help("Moves B up or down, in half screens.");

    Input a = cvIn("A").help("The first trace (any signal; a poly cable shows its first channel).");
    Input b = cvIn("B").help("The second trace; in X-Y, the vertical.");
    Input trigger = gateIn("Trigger").help("With Trigger In, each rising edge starts a sweep.");

    Output sweep = gateOut("Sweep").help("A 1 ms trigger as each sweep starts.");

    // The sweep being drawn: per column, A's low and high, B's low and high.
    SharedArray<float, kPoints * kLanes> live;
    SharedArray<float, kPast * kPoints * kLanes> past;  // the last sweeps, a ring
    Shared<int> filled;                                 // columns of live drawn so far
    Shared<int> newest;                                 // past's slot of the latest finished sweep
    Shared<int> finished;                               // past's slots that hold a sweep
    Shared<int> status;                                 // 0 free, 1 triggered, 2 auto, 3 synced
    SharedArray<float, kTrail> trailA;
    SharedArray<float, kTrail> trailB;
    Shared<int> trailAt;

    bool sweeping = false;
    int position = 0;  // frames into the sweep
    int column = -1;   // the column being filled
    double lastA = 0.0;
    double lastB = 0.0;
    int waited = 0;  // frames since the last sweep ended
    bool armed = false;
    bool triggerHigh = false;
    int lastSpan = -2147483647;
    int pulse = 0;
    bool lowFrame = false;
    bool wasHigh = false;
    int sinceTrail = 0;

    void factoryPatches(Patches& p) {
        p.add("Audio");
        p.add("Slow Modulation").set(timeBase, 4000).set(source, "free");
        p.add("Bar").set(sync, true).set(span, "d1_1");
        p.add("Stacked Pair").set(view, "stacked").set(timeBase, 50);
        p.add("Lissajous").set(view, "xy").set(timeBase, 50).set(persistence, 70);
        p.add("Gates").set(view, "stacked").set(timeBase, 2000).set(source, "free");
    }

    void prepare() {
        for (int k = 0; k < kPoints * kLanes; ++k) live[k] = 0.0f;
        for (int k = 0; k < kPast * kPoints * kLanes; ++k) past[k] = 0.0f;
        for (int k = 0; k < kTrail; ++k) {
            trailA[k] = 0.0f;
            trailB[k] = 0.0f;
        }
        filled.set(0);
        newest.set(0);
        finished.set(0);
        trailAt.set(0);
    }

    void begin(int kind) {
        sweeping = true;
        position = 0;
        column = -1;
        filled.set(0);
        status.set(kind);
        pulse = max(1, roundToInt(0.001 * sampleRate()));
        lowFrame = wasHigh;
    }

    void finish() {
        const int slot = imod(newest.get() + 1, kPast);
        for (int k = 0; k < kPoints * kLanes; ++k) past[slot * kPoints * kLanes + k] = live[k];
        newest.set(slot);
        finished.set(min(kPast, finished.get() + 1));
        filled.set(0);
        sweeping = false;
        waited = 0;
    }

    void put(int k, double va, double vb) {
        const float fa = static_cast<float>(va);
        const float fb = static_cast<float>(vb);
        live[k] = fa;
        live[kPoints + k] = fa;
        live[2 * kPoints + k] = fb;
        live[3 * kPoints + k] = fb;
    }

    // Frame values into column k: the column's low and high, and straight
    // lines through any columns a short screen skips.
    void record(int k, double va, double vb) {
        if (k == column) {
            live[k] = static_cast<float>(min(static_cast<double>(live[k]), va));
            live[kPoints + k] = static_cast<float>(max(static_cast<double>(live[kPoints + k]), va));
            live[2 * kPoints + k] =
                static_cast<float>(min(static_cast<double>(live[2 * kPoints + k]), vb));
            live[3 * kPoints + k] =
                static_cast<float>(max(static_cast<double>(live[3 * kPoints + k]), vb));
        } else {
            if (column >= 0 && k > column + 1)
                for (int j = column + 1; j < k; ++j) {
                    const double f = static_cast<double>(j - column) / (k - column);
                    put(j, lastA + (va - lastA) * f, lastB + (vb - lastB) * f);
                }
            put(k, va, vb);
            column = k;
            filled.set(k + 1);
        }
        lastA = va;
        lastB = vb;
    }

    void process(int frames) {
        sweep.setChannels(1);
        const double sr = sampleRate();
        const int src = clamp(source.index(), 0, 3);
        const bool xy = view.index() == 2;
        const bool falling = slope.index() == 1;
        const Transport t = transport();
        const bool synced = sync.on() && t.playing;
        double spanBeats = 0.25;
        for (int k = 0; k < clamp(span.index(), 0, 6); ++k) spanBeats *= 2.0;
        const double perFrame = t.tempo / 60.0 / sr;
        const int length = max(2, roundToInt(timeBase.value() * 0.001 * sr));
        const int patience = max(length, roundToInt(0.25 * sr));
        const int every = max(1, roundToInt(length / static_cast<double>(kTrail)));
        for (int i = 0; i < frames; ++i) {
            const double va = a[0][i];
            const double vb = b[0][i];

            // The trigger, watched all the time; it starts a sweep only
            // between sweeps.
            bool fire = false;
            if (src == 3) {
                const double g = trigger[0][i];
                if (!triggerHigh && g >= 0.5) {
                    triggerHigh = true;
                    fire = true;
                } else if (triggerHigh && g < 0.25) {
                    triggerHigh = false;
                }
            } else if (src > 0) {
                const double range = src == 1 ? rangeA.at(i) : rangeB.at(i);
                double v = src == 1 ? va : vb;
                double lv = level.at(i);
                if (falling) {
                    v = -v;
                    lv = -lv;
                }
                if (v < lv - 0.01 * range) {
                    armed = true;
                } else if (armed && v >= lv) {
                    armed = false;
                    fire = true;
                }
            }

            if (synced) {
                const double q = (t.beats + i * perFrame + 1e-7) / spanBeats;
                const int s = floorToInt(q);
                if (s != lastSpan) {
                    if (sweeping) finish();
                    begin(3);
                    lastSpan = s;
                }
                record(clamp(floorToInt((q - s) * kPoints), 0, kPoints - 1), va, vb);
            } else {
                if (lastSpan != -2147483647) {
                    lastSpan = -2147483647;
                    if (sweeping) finish();
                }
                if (!sweeping) {
                    ++waited;
                    if (src == 0) {
                        begin(0);
                    } else if (fire) {
                        begin(1);
                    } else if (waited > patience) {
                        begin(2);
                    }
                }
                if (sweeping) {
                    record(min(kPoints - 1, idiv(position * kPoints, length)), va, vb);
                    ++position;
                    if (position >= length) finish();
                }
            }

            if (xy) {
                ++sinceTrail;
                if (sinceTrail >= every) {
                    sinceTrail = 0;
                    const int at = imod(trailAt.get() + 1, kTrail);
                    trailA[at] = static_cast<float>(va);
                    trailB[at] = static_cast<float>(vb);
                    trailAt.set(at);
                }
            }

            bool on = false;
            if (lowFrame) {
                lowFrame = false;
            } else if (pulse > 0) {
                on = true;
                --pulse;
            }
            sweep[0][i] = on ? 1.0 : 0.0;
            wasHigh = on;
        }
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(300, 426);
        p.display(kLeft, kTop, kWide, kHigh);
        p.choice(view, 8, 220, 90, 18);
        p.choice(source, 105, 220, 90, 18);
        p.choice(slope, 202, 220, 90, 18);
        p.knob(timeBase, 42, 272);
        p.knob(level, 114, 272);
        p.knob(persistence, 186, 272);
        p.toggle(sync, 228, 252, 60, 18);
        p.choice(span, 228, 278, 60, 18);
        p.knob(rangeA, 42, 336);
        p.knob(offsetA, 114, 336);
        p.knob(rangeB, 186, 336);
        p.knob(offsetB, 258, 336);
        p.input(a, 42, 398);
        p.input(b, 114, 398);
        p.input(trigger, 186, 398);
        p.output(sweep, 258, 398);
    }

    // Column k of lane `lane` in a sweep: slot -1 is the one being drawn.
    double sample(int slot, int lane, int k) const {
        if (slot < 0) return static_cast<double>(live[lane * kPoints + k]);
        return static_cast<double>(past[(slot * kLanes + lane) * kPoints + k]);
    }

    double toY(double v, double centre, double half, double range, double offset) const {
        const double y = centre - (v / max(0.001, range) + offset) * half;
        return clamp(y, kTop - 8.0, kTop + kHigh + 8.0);
    }

    // The path of one trace over columns [from, to): each column a stroke
    // across it from its low to its high, starting at the end nearer the
    // last column's, so a slow wave is a smooth line and a fast one a solid
    // band.
    void tracePath(Canvas& g, int slot, int pair, int from, int to, double centre, double half,
                   double range, double offset) {
        const double dx = kWide / kPoints;
        double previous = 0.0;
        g.beginPath();
        for (int k = from; k < to; ++k) {
            const double x = kLeft + (k + 0.5) * dx;
            const double lo = toY(sample(slot, 2 * pair, k), centre, half, range, offset);
            const double hi = toY(sample(slot, 2 * pair + 1, k), centre, half, range, offset);
            double first = lo;
            double second = hi;
            if (k > from && abs(previous - hi) < abs(previous - lo)) {
                first = hi;
                second = lo;
            }
            if (k == from)
                g.moveTo(x - 0.5 * dx, first);
            else
                g.lineTo(x - 0.5 * dx, first);
            g.lineTo(x + 0.5 * dx, second);
            previous = second;
        }
    }

    // A trace: its older sweeps faint, then the newest, glowing.
    void drawTrace(Canvas& g, int pair, double centre, double half, int colour) {
        const double range = pair == 0 ? rangeA.value() : rangeB.value();
        const double offset = pair == 0 ? offsetA.value() : offsetB.value();
        const int count = clamp(finished.get(), 0, kPast);
        const int top = clamp(newest.get(), 0, kPast - 1);
        const int head = clamp(filled.get(), 0, kPoints);
        const double keep = clamp(persistence.value(), 0.0, 1.0);
        g.strokeStyle(colour);
        g.lineJoin(LineJoin::Round);
        g.lineCap(LineCap::Round);
        double fade = 0.45 * keep;
        for (int j = count - 1; j >= 1; --j) {
            double alpha = fade;
            for (int q = 1; q < j; ++q) alpha *= keep;
            if (alpha < 0.03) continue;
            g.globalAlpha(alpha);
            g.lineWidth(1.0);
            tracePath(g, imod(top - j, kPast), pair, 0, kPoints, centre, half, range, offset);
            g.stroke();
        }
        // Twice: a wide soft glow, then the line.
        for (int pass = 0; pass < 2; ++pass) {
            g.globalAlpha(pass == 0 ? 0.16 : 1.0);
            g.lineWidth(pass == 0 ? 5.0 : 1.4);
            if (count > 0 && head < kPoints) {
                tracePath(g, top, pair, head, kPoints, centre, half, range, offset);
                g.stroke();
            }
            if (head > 0) {
                tracePath(g, -1, pair, 0, head, centre, half, range, offset);
                g.stroke();
            }
        }
        // The write head, on a slow sweep.
        if (head > 0 && head < kPoints) {
            const double x = kLeft + (head - 0.5) * kWide / kPoints;
            const double y = toY(sample(-1, 2 * pair + 1, head - 1), centre, half, range, offset);
            g.globalAlpha(0.25);
            g.fillStyle(colour);
            g.beginPath();
            g.circle(x, y, 4.5);
            g.fill();
            g.globalAlpha(1.0);
            g.beginPath();
            g.circle(x, y, 1.8);
            g.fill();
        }
        g.globalAlpha(1.0);
    }

    void graticule(Canvas& g, bool stacked) {
        const double cx = kLeft + kWide / 2.0;
        const double cy = kTop + kHigh / 2.0;
        g.lineWidth(1.0);
        g.strokeStyle(Theme::line);
        g.globalAlpha(0.55);
        g.beginPath();
        for (int k = 1; k < 10; ++k) {
            const double x = kLeft + kWide * k / 10.0;
            g.moveTo(x, kTop + 1);
            g.lineTo(x, kTop + kHigh - 1);
        }
        for (int k = 1; k < 8; ++k) {
            const double y = kTop + kHigh * k / 8.0;
            g.moveTo(kLeft + 1, y);
            g.lineTo(kLeft + kWide - 1, y);
        }
        g.stroke();
        // The axes, with fine ticks a fifth of a division apart.
        g.globalAlpha(0.9);
        g.strokeStyle(Theme::lineLit);
        g.beginPath();
        g.moveTo(cx, kTop + 1);
        g.lineTo(cx, kTop + kHigh - 1);
        for (int k = 1; k < 50; ++k) {
            const double x = kLeft + kWide * k / 50.0;
            const double y = stacked ? kTop + kHigh / 4.0 : cy;
            g.moveTo(x, y - 2);
            g.lineTo(x, y + 2);
            if (stacked) {
                g.moveTo(x, y + kHigh / 2.0 - 2);
                g.lineTo(x, y + kHigh / 2.0 + 2);
            }
        }
        for (int k = 1; k < 40; ++k) {
            const double y = kTop + kHigh * k / 40.0;
            g.moveTo(cx - 2, y);
            g.lineTo(cx + 2, y);
        }
        if (stacked) {
            g.moveTo(kLeft + 1, kTop + kHigh / 4.0);
            g.lineTo(kLeft + kWide - 1, kTop + kHigh / 4.0);
            g.moveTo(kLeft + 1, kTop + kHigh * 0.75);
            g.lineTo(kLeft + kWide - 1, kTop + kHigh * 0.75);
        } else {
            g.moveTo(kLeft + 1, cy);
            g.lineTo(kLeft + kWide - 1, cy);
        }
        g.stroke();
        if (stacked) {
            g.strokeStyle(Theme::line);
            g.beginPath();
            g.moveTo(kLeft + 1, cy);
            g.lineTo(kLeft + kWide - 1, cy);
            g.stroke();
        }
        g.globalAlpha(1.0);
    }

    void drawTrail(Canvas& g) {
        const double cx = kLeft + kWide / 2.0;
        const double cy = kTop + kHigh / 2.0;
        const double half = kHigh / 2.0 - 4.0;
        const double ra = max(0.001, rangeA.value());
        const double rb = max(0.001, rangeB.value());
        const double oa = offsetA.value();
        const double ob = offsetB.value();
        const int at = imod(trailAt.get(), kTrail);
        const int count =
            clamp(48 + roundToInt(clamp(persistence.value(), 0.0, 1.0) * (kTrail - 48)), 2, kTrail);
        g.lineJoin(LineJoin::Round);
        g.lineCap(LineCap::Round);
        // The glow under the whole trail, then each segment fading with age.
        g.strokeStyle(Theme::accent);
        g.globalAlpha(0.12);
        g.lineWidth(5.0);
        g.beginPath();
        for (int j = 0; j < count; ++j) {
            const int k = imod(at - count + 1 + j, kTrail);
            const double x = cx + (static_cast<double>(trailA[k]) / ra + oa) * half;
            const double y = cy - (static_cast<double>(trailB[k]) / rb + ob) * half;
            if (j == 0)
                g.moveTo(x, y);
            else
                g.lineTo(x, y);
        }
        g.stroke();
        g.lineWidth(1.4);
        for (int j = 1; j < count; ++j) {
            const int k0 = imod(at - count + j, kTrail);
            const int k1 = imod(at - count + 1 + j, kTrail);
            const double age = static_cast<double>(j) / count;
            g.globalAlpha(0.06 + 0.94 * age * age);
            g.beginPath();
            g.moveTo(cx + (static_cast<double>(trailA[k0]) / ra + oa) * half,
                     cy - (static_cast<double>(trailB[k0]) / rb + ob) * half);
            g.lineTo(cx + (static_cast<double>(trailA[k1]) / ra + oa) * half,
                     cy - (static_cast<double>(trailB[k1]) / rb + ob) * half);
            g.stroke();
        }
        const double hx = cx + (static_cast<double>(trailA[at]) / ra + oa) * half;
        const double hy = cy - (static_cast<double>(trailB[at]) / rb + ob) * half;
        g.fillStyle(Theme::text);
        g.globalAlpha(0.3);
        g.beginPath();
        g.circle(hx, hy, 4.5);
        g.fill();
        g.globalAlpha(1.0);
        g.beginPath();
        g.circle(hx, hy, 2.0);
        g.fill();
    }

    void draw(Canvas& g) {
        g.well(kLeft, kTop, kWide, kHigh);
        const int v = clamp(view.index(), 0, 2);
        const bool stacked = v == 1;
        graticule(g, stacked);
        const double cy = kTop + kHigh / 2.0;
        const int src = clamp(source.index(), 0, 3);
        g.save();
        g.clip(kLeft + 1, kTop + 1, kWide - 2, kHigh - 2);
        if (v == 2) {
            drawTrail(g);
        } else {
            const double half = stacked ? kHigh / 4.0 - 3.0 : kHigh / 2.0 - 4.0;
            const double centreA = stacked ? kTop + kHigh / 4.0 : cy;
            const double centreB = stacked ? kTop + kHigh * 0.75 : cy;
            drawTrace(g, 1, centreB, half, Theme::accent2);
            drawTrace(g, 0, centreA, half, Theme::accent);
            // The trigger level: a marker on the left edge and a faint line.
            if ((src == 1 || src == 2) && !(sync.on() && status.get() == 3)) {
                const bool onA = src == 1;
                const double y = toY(level.value(), onA ? centreA : centreB, half,
                                     onA ? rangeA.value() : rangeB.value(),
                                     onA ? offsetA.value() : offsetB.value());
                const int colour = onA ? Theme::accent : Theme::accent2;
                g.strokeStyle(colour);
                g.globalAlpha(0.22);
                g.lineWidth(1.0);
                g.beginPath();
                g.moveTo(kLeft + 1, y);
                g.lineTo(kLeft + kWide - 1, y);
                g.stroke();
                g.globalAlpha(1.0);
                g.fillStyle(colour);
                g.beginPath();
                g.moveTo(kLeft + 1, y - 4.5);
                g.lineTo(kLeft + 8, y);
                g.lineTo(kLeft + 1, y + 4.5);
                g.closePath();
                g.fill();
            }
        }
        g.restore();

        // Readouts: the state, the screen's length, and the ranges.
        const int st = status.get();
        g.monoFont(9);
        g.textBaseline(TextBaseline::Top);
        g.textAlign(TextAlign::Left);
        g.fillStyle(st == 1 || st == 3 ? Theme::accent : Theme::textFaint);
        if (v == 2)
            g.fillText("X-Y", kLeft + 6, kTop + 5);
        else
            g.fillText(st == 0 ? "FREE" : (st == 1 ? "TRIG'D" : (st == 2 ? "AUTO" : "SYNC")),
                       kLeft + 6, kTop + 5);
        g.fillStyle(Theme::textMuted);
        g.textBaseline(TextBaseline::Bottom);
        g.fillText(sync.on() && st == 3 && v != 2 ? span.text() : timeBase.text(), kLeft + 6,
                   kTop + kHigh - 4);
        g.textAlign(TextAlign::Right);
        g.fillStyle(Theme::accent2);
        g.fillText(format("B %.2f", rangeB.value()), kLeft + kWide - 6, kTop + kHigh - 4);
        g.textBaseline(TextBaseline::Top);
        g.fillStyle(Theme::accent);
        g.fillText(format("A %.2f", rangeA.value()), kLeft + kWide - 6, kTop + 5);
    }
};
