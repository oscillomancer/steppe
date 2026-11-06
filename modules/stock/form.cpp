// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// The shape of a song over minutes: sections of Bars each, in a plan, so a
// patch can build, drop and change instead of looping. Verse, Chorus
// plays intro, verse, chorus, verse, chorus, bridge, chorus, outro; AABA
// a verse three times round a bridge; Build, Drop an intro, a break, a
// build and a drop, twice, and an outro; Wander an intro and then a walk
// of verses, choruses and bridges drawn from the seed that never ends.
// Intros and outros are half as long. A, B and C are gates for the
// sections' roles (verse or break; chorus or drop; bridge or build), Edge
// for the intro and outro: patch them to mutes, switches and sequencers'
// resets. Energy is each section's level, rising through intros and
// builds, falling through outros: patch it to a filter, a density or a
// mixer. Fill is high for the last Fill of each section, for a drum fill;
// Progress rises 0 to 1 through each; Change fires as each begins and End
// as the form ends (with Loop off it then rests, every gate low). Each bar
// is the song's, or an edge in Bar; Next jumps to the next section's
// start, and Reset makes now the form's start.

constexpr int kFormSlots = 32;

// A section's kind: 0 intro, 1 verse, 2 chorus, 3 bridge, 4 outro,
// 5 build, 6 drop, 7 break.
int formKind(int plan, int s) {
    switch (plan) {
        case 1:  // AABA
            return s == 2 ? 3 : 1;
        case 2:  // build, drop
            switch (s) {
                case 0:
                    return 0;
                case 1:
                case 4:
                    return 7;
                case 2:
                case 5:
                    return 5;
                case 3:
                case 6:
                    return 6;
                default:
                    return 4;
            }
        default:  // verse, chorus
            switch (s) {
                case 0:
                    return 0;
                case 1:
                case 3:
                    return 1;
                case 2:
                case 4:
                case 6:
                    return 2;
                case 5:
                    return 3;
                default:
                    return 4;
            }
    }
}

// Its role: 0 Edge, 1 A, 2 B, 3 C.
int formRole(int kind) {
    switch (kind) {
        case 1:
        case 7:
            return 1;
        case 2:
        case 6:
            return 2;
        case 3:
        case 5:
            return 3;
        default:
            return 0;
    }
}

// Its energy, `p` of the way through it.
double formEnergy(int kind, double p) {
    switch (kind) {
        case 0:
            return 0.2 + 0.2 * p;
        case 1:
            return 0.5;
        case 2:
            return 0.85;
        case 3:
            return 0.4;
        case 4:
            return 0.5 * (1.0 - p);
        case 5:
            return 0.3 + 0.7 * p;
        case 6:
            return 1.0;
        default:
            return 0.25;
    }
}

struct Form : Module {
    static constexpr ModuleInfo info = {
        .name = "Form",
        .id = "com.suppleaudio.rack.form",
        .vendor = "Supple Audio",
        .description = "A song's sections over minutes: gates, an energy arc and fills.",
        .tags = {"sequencer", "generative", "clock", "utility"}};

    Param plan = choice("Plan", {"Verse, Chorus", "AABA", "Build, Drop", "Wander"}, 0)
                     .keys({"verse_chorus", "aaba", "build_drop", "wander"})
                     .help("The order of the sections.");
    Param bars = choice("Bars", {"4", "8", "16"}, 1)
                     .keys({"bars_4", "bars_8", "bars_16"})
                     .help("How many bars each section lasts; intros and outros half as many.");
    Param fill_length = choice("Fill", {"1 Beat", "2 Beats", "1 Bar"}, 2)
                            .keys({"beat_1", "beats_2", "bar_1"})
                            .help("How long Fill is high at the end of each section.");
    Param loop =
        toggle("Loop", true).help("Start the form over as it ends; off, it rests after the end.");

    Input bar =
        gateIn("Bar").help("Each rising edge is a bar; with nothing here, the song's bars.");
    Input next = gateIn("Next").help("A rising edge jumps to the next section's start.");
    Input reset = gateIn("Reset").help("A rising edge makes now the form's start.");

    Output a = gateOut("A").help("High through verses and breaks.");
    Output b = gateOut("B").help("High through choruses and drops.");
    Output c = gateOut("C").help("High through bridges and builds.");
    Output edge = gateOut("Edge").help("High through the intro and the outro.");
    Output energy = unipolarOut("Energy").help(
        "The section's level: rising through intros and builds, falling through outros.");
    Output progress = phaseOut("Progress").help("0 to 1 through each section.");
    Output fill = gateOut("Fill").help("High for the last Fill of each section.");
    Output change = gateOut("Change").help("A trigger as each section begins.");
    Output end = gateOut("End").help("A trigger as the form ends.");

    Array<int, kFormSlots> wander;  // Wander's sections, drawn from the seed
    Random rng;
    Trigger barEdge;
    Trigger nextEdge;
    Trigger resetEdge;
    Pulse changePulse;
    Pulse endPulse;
    int salt = 0;
    bool started = false;
    double offset = 0.0;  // bars added to the song's or the count
    int counted = -1;     // bars counted on Bar
    int sinceEdge = 0;    // frames since the last edge in Bar
    int period = 0;       // frames between the last two
    int lastSection = -1;
    int lastRound = 0;
    bool rested = false;

    Shared<int> shownPlan;
    Shared<int> shownAt;
    Shared<float> shownProgress;
    SharedArray<int, kFormSlots> shownWander;

    void factoryPatches(Patches& p) {
        p.add("Pop Song");
        p.add("Standard").set(plan, "aaba").set(fill_length, "beats_2");
        p.add("Club").set(plan, "build_drop").set(bars, "bars_16");
        p.add("Endless").set(plan, "wander");
        p.add("Short Sections").set(bars, "bars_4").set(fill_length, "beat_1");
    }

    void prepare() {
        salt = seed();
        started = false;  // drawn on the first block
    }

    void startOver() {
        rng.seed(salt);
        // From the intro, a walk: a verse leads most to a chorus, a chorus
        // back to a verse or on to a bridge, a bridge to a chorus.
        wander[0] = 0;
        int k = 1;
        for (int s = 1; s < kFormSlots; ++s) {
            const double u = rng.next();
            if (k == 2)
                k = u < 0.5 ? 1 : (u < 0.8 ? 3 : 2);
            else if (k == 3)
                k = u < 0.7 ? 2 : 1;
            else
                k = u < 0.6 ? 2 : (u < 0.85 ? 1 : 3);
            if (s == 1) k = 1;
            wander[s] = k;
        }
    }

    int count() const {
        switch (plan.index()) {
            case 1:
                return 4;
            case 3:
                return kFormSlots;
            default:
                return 8;
        }
    }
    int kindAt(int s) const { return plan.index() == 3 ? wander[s] : formKind(plan.index(), s); }
    double sectionBars(int s) const {
        const int k = bars.index();
        const double full = k == 0 ? 4.0 : (k == 2 ? 16.0 : 8.0);
        const int kind = kindAt(s);
        return kind == 0 || kind == 4 ? full / 2.0 : full;
    }
    double totalBars() const {
        double sum = 0.0;
        for (int s = 0; s < count(); ++s) sum += sectionBars(s);
        return sum;
    }

    // Where `pos` bars into the form falls: its section, and the bars into it.
    int sectionOf(double pos, double& into) const {
        double start = 0.0;
        const int n = count();
        for (int s = 0; s < n; ++s) {
            const double len = sectionBars(s);
            if (pos < start + len) {
                into = pos - start;
                return s;
            }
            start += len;
        }
        into = sectionBars(n - 1);
        return n - 1;
    }

    void process(int frames) {
        if (!started) {
            started = true;
            startOver();
        }
        const Transport t = transport();
        const bool internal = !bar.connected();
        const double barBeats = t.timeSigNumerator * 4.0 / max(1, t.timeSigDenominator);
        const double beatsPerFrame = t.tempo / 60.0 / sampleRate();
        const double total = totalBars();
        const double fillBars =
            fill_length.index() == 0 ? 0.25 : (fill_length.index() == 1 ? 0.5 : 1.0);
        int section = 0;
        double into = 0.0;
        for (int i = 0; i < frames; ++i) {
            // Bars since the start: the song's, or Bar's edges with the
            // fraction of the last bar's length gone since the latest.
            double song = 0.0;
            if (internal) {
                song = (t.beats + i * beatsPerFrame) / max(0.25, barBeats);
            } else {
                ++sinceEdge;
                if (barEdge.rose(bar[0][i])) {
                    if (counted >= 0) period = sinceEdge;
                    sinceEdge = 0;
                    ++counted;
                }
                const double part =
                    period > 0 ? min(0.999, static_cast<double>(sinceEdge) / period) : 0.0;
                song = counted < 0 ? 0.0 : counted + part;
            }
            if (resetEdge.rose(reset[0][i])) {
                offset = internal ? -floor(song + 1e-9) : 0.0;
                if (!internal) {
                    counted = -1;
                    song = 0.0;
                }
                lastSection = -1;
                lastRound = 0;
                rested = false;
            }
            double pos = song + offset + 1e-9;
            if (nextEdge.rose(next[0][i])) {
                double in = 0.0;
                const double p = pos - total * floor(pos / total);
                const int s = sectionOf(p, in);
                offset += sectionBars(s) - in;
                pos = song + offset + 1e-9;
            }
            // Wander goes on past its last slot from the first after the
            // intro, and never ends.
            const bool endless = plan.index() == 3;
            const double intro = sectionBars(0);
            double inForm = pos - total * floor(pos / total);
            if (endless && pos >= total)
                inForm = intro + (pos - intro) -
                         (total - intro) * floor((pos - intro) / (total - intro));
            const int round = endless ? 0 : floorToInt(pos / total);
            const bool over = !loop.on() && round >= 1;
            if (round != lastRound && pos >= 0.0) {
                if (!rested) endPulse.fire();
                rested = over;
                lastRound = round;
            }
            section = sectionOf(inForm, into);
            if (section != lastSection && !over) {
                changePulse.fire();
                lastSection = section;
            }
            const double len = sectionBars(section);
            const double p = clamp(into / len, 0.0, 1.0);
            const int kind = kindAt(section);
            const int role = formRole(kind);
            a[0][i] = !over && role == 1 ? 1.0 : 0.0;
            b[0][i] = !over && role == 2 ? 1.0 : 0.0;
            c[0][i] = !over && role == 3 ? 1.0 : 0.0;
            edge[0][i] = !over && role == 0 ? 1.0 : 0.0;
            energy[0][i] = over ? 0.0 : formEnergy(kind, p);
            progress[0][i] = over ? 0.0 : p;
            fill[0][i] = !over && len - into <= fillBars + 1e-9 ? 1.0 : 0.0;
            change[0][i] = changePulse.next();
            end[0][i] = endPulse.next();
            if (over) section = -1;
        }
        shownPlan.set(plan.index());
        shownAt.set(section);
        shownProgress.set(static_cast<float>(clamp(into / sectionBars(max(0, section)), 0.0, 1.0)));
        for (int s = 0; s < kFormSlots; ++s) shownWander[s] = wander[s];
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(220, 336);
        p.display(8, 28, 204, 96);
        p.choice(plan, 8, 132, 204, 18);
        p.choice(bars, 8, 156, 98, 18);
        p.choice(fill_length, 114, 156, 98, 18);
        p.input(bar, 26, 204);
        p.input(next, 68, 204);
        p.input(reset, 110, 204);
        p.toggle(loop, 140, 196, 72, 16);
        p.output(a, 26, 254);
        p.output(b, 68, 254);
        p.output(c, 110, 254);
        p.output(edge, 152, 254);
        p.output(energy, 26, 304);
        p.output(progress, 68, 304);
        p.output(fill, 110, 304);
        p.output(change, 152, 304);
        p.output(end, 194, 304);
    }

    int shownKind(int s) const {
        const int pl = shownPlan.get();
        return pl == 3 ? shownWander[s] : formKind(pl, s);
    }

    // Up to eight sections as blocks, as wide as they are long and as tall
    // as their energy (slopes where it rises or falls), a letter under each;
    // the one playing bright, with the place in it.
    void draw(Canvas& g) {
        g.well(8, 28, 204, 96);
        const int pl = shownPlan.get();
        const int at = shownAt.get();
        const int n = pl == 1 ? 4 : 8;
        const int first = pl == 3 ? (at < 0 ? 0 : at - imod(at, 8)) : 0;
        double sum = 0.0;
        for (int s = 0; s < n; ++s) {
            const int k = shownKind(first + s);
            sum += k == 0 || k == 4 ? 0.5 : 1.0;
        }
        const double x0 = 12.0;
        const double w = 196.0;
        const double base = 106.0;
        const double tall = 70.0;
        double x = x0;
        g.font(9.0);
        g.textAlign(TextAlign::Center);
        for (int s = 0; s < n; ++s) {
            const int k = shownKind(first + s);
            const double bw = w * (k == 0 || k == 4 ? 0.5 : 1.0) / sum;
            const bool now = first + s == at;
            g.fillStyle(formRole(k) == 2 ? Theme::accent : Theme::text);
            g.globalAlpha(now ? 0.9 : 0.35);
            g.beginPath();
            g.moveTo(x + 1.0, base);
            g.lineTo(x + 1.0, base - tall * formEnergy(k, 0.0));
            g.lineTo(x + bw - 1.0, base - tall * formEnergy(k, 1.0));
            g.lineTo(x + bw - 1.0, base);
            g.closePath();
            g.fill();
            g.globalAlpha(1.0);
            g.fillStyle(now ? Theme::text : Theme::line);
            const double cx = x + bw / 2.0;
            switch (k) {
                case 0:
                    g.fillText("In", cx, 117.0);
                    break;
                case 1:
                    g.fillText("V", cx, 117.0);
                    break;
                case 2:
                    g.fillText("C", cx, 117.0);
                    break;
                case 3:
                    g.fillText("Br", cx, 117.0);
                    break;
                case 4:
                    g.fillText("Out", cx, 117.0);
                    break;
                case 5:
                    g.fillText("Up", cx, 117.0);
                    break;
                case 6:
                    g.fillText("Dr", cx, 117.0);
                    break;
                default:
                    g.fillText("Bk", cx, 117.0);
                    break;
            }
            if (now) {
                const double px = x + 1.0 + (bw - 2.0) * static_cast<double>(shownProgress.get());
                g.strokeStyle(Theme::text);
                g.lineWidth(1.0);
                g.beginPath();
                g.moveTo(px, 32.0);
                g.lineTo(px, base);
                g.stroke();
            }
            x += bw;
        }
    }
};
