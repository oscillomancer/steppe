// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A glitch effect, poly: at each step, on Clock or on the song's beat grid,
// it may break the sound up for that step: stutter a slice of what just
// played, play it backwards, play it slower and lower, or wind it down like
// a tape stopping. Chance sets how often, the four weights which; Length
// the slice in note values at the song's tempo, which Vary sometimes halves
// (or quarters, or eighths) for rolls and fills. Between glitches the sound
// passes untouched.
//
// The choices come from the module's seed, the same at every render; Reset
// starts them again, so a pattern can repeat with a phrase. Active is high
// while a glitch plays. Every voice of a poly input glitches together.
// Joins are smoothed: 3 ms between steps, 2 ms at each repeat's edges.

constexpr double kBufferSeconds = 6.0;
constexpr double kLongestSlice = 1.5;  // seconds
constexpr double kLongestHold = 4.0;   // a glitch plays for at most this long
constexpr double kJoinMs = 3.0;
constexpr double kEdgeMs = 2.0;

constexpr int kLive = 0;
constexpr int kStutter = 1;
constexpr int kReverse = 2;
constexpr int kDrop = 3;
constexpr int kStop = 4;

// A note value's length in quarter notes, from 1/64 to 1/2.
double noteOf(int option) { return 0.0625 * exp2(1.0 * clamp(option, 0, 5)); }

struct Step {
    int mode = kLive;
    double t = 0.0;      // samples since the step began
    double slice = 1.0;  // the repeated slice, in samples
    double speed = 1.0;  // a drop's
    double span = 1.0;   // the step's expected length, in samples: a stop's
};

struct Glitch : Module {
    static constexpr ModuleInfo info = {
        .name = "Glitch",
        .id = "com.suppleaudio.rack.glitch",
        .vendor = "Supple Audio",
        .description =
            "A clocked glitch: stutters, reverses, pitch drops and tape stops, seeded, "
            "with a reset.",
        .tags = {"effect", "rhythm", "poly"}};

    Param chance =
        percent("Chance", 35).in("Steps").help("How often a step glitches: 100 is every one.");
    Param grid = choice("Grid", {"1/16", "1/8", "1/4", "1/2", "1 bar"}, 2)
                     .keys({"n16", "n8", "n4", "n2", "bar"})
                     .in("Steps")
                     .help("The steps at the song's tempo, while nothing is patched to Clock.");
    Param length = choice("Length", {"1/64", "1/32", "1/16", "1/8", "1/4", "1/2"}, 2)
                       .keys({"n64", "n32", "n16", "n8", "n4", "n2"})
                       .in("Slice")
                       .help("The slice a glitch repeats, in note values at the song's tempo.");
    Param vary = percent("Vary", 30)
                     .in("Slice")
                     .help("How often the slice is halved, or more, for rolls: 0 keeps Length.");
    Param stutter =
        percent("Stutter", 100).in("Kinds").help("How likely a glitch repeats the slice.");
    Param reverse =
        percent("Reverse", 50).in("Kinds").help("How likely it plays the slice backwards.");
    Param drop =
        percent("Drop", 40)
            .in("Kinds")
            .help("How likely it repeats the slice slower: a fourth, a fifth or an octave down.");
    Param stop = percent("Stop", 25)
                     .in("Kinds")
                     .help("How likely it winds down over the step, as a tape stops.");
    Param mix = percent("Mix", 100)
                    .smoothed(20)
                    .in("Output")
                    .help("The glitched share, the rest the dry input.");

    Input in = audioIn("In").help("What to glitch. Poly: every voice glitches together.");
    Input clock =
        gateIn("Clock").help("A step at each rise; while empty, Grid at the song's tempo.");
    Input reset = gateIn("Reset").help("Starts the choices again from the seed, and a step.");

    Output out = audioOut("Out").help("The input, glitched.");
    Output active = gateOut("Active").help("High while a glitch plays.");

    Array<DelayLine, 16> line;
    Random random;
    Step now, before;        // this step, and the last while the join fades
    double join = 1.0;       // 0 to 1 across the join
    double freeBeats = 0.0;  // the song position while the transport is stopped
    int lastIndex = -1;
    bool clockHigh = false, resetHigh = false;
    double sinceClock = 0.0, clockPeriod = 0.0;

    void prepare() {
        const int size = toInt(kBufferSeconds * sampleRate()) + 8;
        for (int c = 0; c < 16; ++c) line[c].prepare(size);
        random.seed(seed());
    }

    // How far back a step reads at its time t, and its gain there.
    double delayOf(const Step& s) const {
        const double t = s.t;
        if (s.mode == kStutter) return s.slice + s.slice * floor(t / s.slice);
        if (s.mode == kReverse) return t + (t - s.slice * floor(t / s.slice)) + 1.0;
        if (s.mode == kDrop) {
            const double turn = s.slice / s.speed;
            return s.slice + t - s.speed * (t - turn * floor(t / turn));
        }
        if (s.mode == kStop) return min(t, s.span) * min(t, s.span) / (2.0 * s.span);
        return 0.0;
    }
    double gainOf(const Step& s, double edge) const {
        if (s.mode == kLive) return 1.0;
        if (s.mode == kStop) return clamp((s.span - s.t) / (0.2 * s.span), 0.0, 1.0);
        // Each repeat fades in and out at its edges.
        const double turn = s.mode == kDrop ? s.slice / s.speed : s.slice;
        const double into = s.t - turn * floor(s.t / turn);
        return clamp(min(into, turn - into) / edge, 0.0, 1.0);
    }

    // A new step: perhaps a glitch, and which.
    void begin(double span) {
        const double sr = sampleRate();
        const double beat = 60.0 / max(transport().tempo, 1.0) * sr;
        before = now;
        join = 0.0;
        const Step fresh;
        now = fresh;
        now.span = max(span, 1.0);
        const double roll = random.next();
        const double kind = random.next();
        const double halve = random.next();
        const double deeper = random.next();
        const double pitch = random.next();
        if (roll >= chance.value()) return;
        const double ws = stutter.value(), wr = reverse.value(), wd = drop.value(),
                     wt = stop.value();
        const double total = ws + wr + wd + wt;
        if (total <= 0.0) return;
        const double pick = kind * total;
        now.mode = pick < ws ? kStutter
                             : (pick < ws + wr ? kReverse : (pick < ws + wr + wd ? kDrop : kStop));
        int halvings = 0;
        if (halve < vary.value()) halvings = 1 + toInt(deeper * 3.0);
        now.slice = clamp(noteOf(length.index()) * beat / exp2(halvings), 16.0, kLongestSlice * sr);
        now.speed = pitch < 1.0 / 3.0 ? 0.75 : (pitch < 2.0 / 3.0 ? 2.0 / 3.0 : 0.5);
    }

    void process(int frames) {
        const double sr = sampleRate();
        const Transport tr = transport();
        const double perSample = max(tr.tempo, 1.0) / 60.0 / sr;
        const double stepBeats = grid.index() == 4 ? 4.0 : 0.25 * exp2(grid.index());
        const double gridSpan = stepBeats / perSample;
        const double joinStep = 1.0 / (kJoinMs * 0.001 * sr);
        const double edge = kEdgeMs * 0.001 * sr;
        const bool clocked = clock.connected();
        active.setChannels(1);
        for (int i = 0; i < frames; ++i) {
            const double r = reset[0][i];
            const bool resetRises = !resetHigh && r >= 0.5;
            resetHigh = resetHigh ? r >= 0.25 : r >= 0.5;
            const double k = clock[0][i];
            const bool clockRises = !clockHigh && k >= 0.5;
            clockHigh = clockHigh ? k >= 0.25 : k >= 0.5;
            sinceClock += 1.0;
            if (resetRises) {
                random.seed(seed());
                lastIndex = -1;
                freeBeats = 0.0;
            }
            bool starts = resetRises;
            double span = gridSpan;
            if (clocked) {
                if (clockRises) {
                    if (sinceClock < 10.0 * sr) clockPeriod = sinceClock;
                    sinceClock = 0.0;
                    starts = true;
                }
                if (clockPeriod > 0.0) span = clockPeriod;
            } else {
                const double beats = tr.playing ? tr.beats + i * perSample : freeBeats;
                const int index = floorToInt(beats / stepBeats + 1e-9);
                if (index != lastIndex) starts = true;
                lastIndex = index;
            }
            if (!tr.playing) freeBeats += perSample;
            if (starts) begin(span);
            // A glitch held past its longest gives the sound back.
            if (now.mode != kLive && now.t > kLongestHold * sr) {
                before = now;
                join = 0.0;
                const Step fresh;
                now = fresh;
            }
            const double most = kBufferSeconds * sr;
            const double dNow = min(delayOf(now), most), gNow = gainOf(now, edge);
            const double dBefore = min(delayOf(before), most), gBefore = gainOf(before, edge);
            const double m = clamp(mix.at(i, 0), 0.0, 1.0);
            for (int c = 0; c < channels(); ++c) {
                const double x = in[c][i];
                line[c].push(x);
                double y = gNow * line[c].read(dNow);
                if (join < 1.0) y = lerp(gBefore * line[c].read(dBefore), y, join);
                out[c][i] = m * y + (1.0 - m) * x;
            }
            active[0][i] = now.mode != kLive ? 1.0 : 0.0;
            now.t += 1.0;
            before.t += 1.0;
            join = min(join + joinStep, 1.0);
        }
    }

    void factoryPatches(Patches& p) {
        p.add("Rolls")
            .set(chance, 40)
            .set(stutter, 100)
            .set(reverse, 0)
            .set(drop, 0)
            .set(stop, 0)
            .set(vary, 70);
        p.add("Backwards")
            .set(chance, 50)
            .set(stutter, 20)
            .set(reverse, 100)
            .set(drop, 0)
            .set(stop, 0);
        p.add("Tape Stops")
            .set(chance, 25)
            .set(grid, "bar")
            .set(stutter, 0)
            .set(reverse, 0)
            .set(drop, 0)
            .set(stop, 100);
        p.add("Broken Beat").set(chance, 60).set(grid, "n8").set(length, "n32").set(vary, 50);
        p.add("Sparse").set(chance, 12).set(grid, "n2");
    }
};
