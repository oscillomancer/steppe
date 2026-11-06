// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A compressor, poly: turns the sound down by Ratio as its peaks rise above
// Thresh, over a Knee that eases it in, as fast as Attack and letting go as
// slowly as Release. Patch Sidechain and it listens there instead: a kick
// ducking a bass or a pad, a voice ducking music. SC Filter keeps the low
// end out of what it hears, so bass does not pump it. Linked, every
// channel is turned down together (a stereo pair or a poly voice section
// keeps its balance); unlinked, each alone. Gain is the gain it applies,
// 1 at rest, to turn down something else by the same. Makeup brings the
// level back up.

constexpr int kChannels = 16;

struct Detector {
    OnePole low;           // the sidechain filter
    double reduction = 0;  // dB, 0 or below, smoothed
};

struct Compressor : Module {
    static constexpr ModuleInfo info = {
        .name = "Compressor",
        .id = "com.suppleaudio.rack.compressor",
        .vendor = "Supple Audio",
        .description =
            "A compressor: threshold, ratio, knee, attack, release, makeup; a sidechain "
            "to duck by, linked or per channel.",
        .tags = {"dynamics", "effect", "poly"}};

    Param threshold = decibels("Thresh", -60, 0, -18)
                          .in("Curve")
                          .help("Where it starts turning down: peaks above it are reduced.");
    Param slope = ratio("Ratio", 1, 20, 4)
                      .withKey("ratio")
                      .in("Curve")
                      .help("How much: at 4, a peak 8 dB over comes out 2 dB over.");
    Param knee = decibels("Knee", 0, 24, 6)
                     .in("Curve")
                     .help("How gently it eases in around Thresh: 0 is a hard corner.");
    Param attack = milliseconds("Attack", 0.1, 100, 10)
                       .in("Timing")
                       .help("How fast it turns down: slower lets a drum's snap through.");
    Param release = milliseconds("Release", 10, 2000, 150)
                        .in("Timing")
                        .help("How fast it lets go: fast pumps, slow holds the level steady.");
    Param scFilter = frequency("SC Filter", 20, 500, 20)
                         .in("Sidechain")
                         .help("A high pass on what it hears, so bass does not pump it.");
    Param link = toggle("Link", true)
                     .in("Sidechain")
                     .help("Turns every channel down together, by the loudest.");
    Param makeup = decibels("Makeup", 0, 24, 0)
                       .smoothed(20)
                       .in("Output")
                       .help("Gain after: brings the level back up.");
    Param mix = percent("Mix", 100)
                    .smoothed(20)
                    .in("Output")
                    .help("The compressed share, the rest dry: parallel compression below 100.");

    Input in = audioIn("In").help("What to compress. Poly: each channel, linked or alone.");
    Input sidechain = audioIn("Key").help(
        "What it listens to instead of In: a kick to duck by. A mono cable keys every channel.");
    Output out = audioOut("Out").help("The compressed sound.");
    Output gain = unipolarOut("Gain").help(
        "The gain it applies, 1 at rest: into a VCA, it ducks something else alike.");

    Array<Detector, kChannels> detector;
    Shared<double> shownReduction;  // dB, the deepest this block

    // The gain change, in dB (0 or below), for a level in dB.
    double curve(double levelDb, double t, double r, double w) const {
        const double over = levelDb - t;
        const double k = 1.0 / r - 1.0;
        if (2.0 * over <= -w) return 0.0;
        if (2.0 * over < w) return k * (over + 0.5 * w) * (over + 0.5 * w) / (2.0 * w);
        return k * over;
    }

    void process(int frames) {
        const double sr = sampleRate();
        const int n = channels();
        const bool keyed = sidechain.connected();
        const bool linked = link.on();
        gain.setChannels(linked ? 1 : n);
        for (int c = 0; c < n; ++c) detector[c].low.setCutoff(scFilter.value());
        const double up = exp(-1.0 / (max(attack.value(), 0.1) * 0.001 * sr));
        const double down = exp(-1.0 / (max(release.value(), 10.0) * 0.001 * sr));
        const double t = threshold.value(), r = max(slope.value(), 1.0), w = max(knee.value(), 0.0);
        double deepest = 0.0;
        for (int i = 0; i < frames; ++i) {
            double loudest = -200.0;
            for (int c = 0; c < n; ++c) {
                const double key = keyed ? sidechain[c][i] : in[c][i];
                const double heard = scFilter.value() > 20.5 ? detector[c].low.highpass(key) : key;
                const double levelDb = 20.0 * log10(max(abs(heard), 1e-10));
                if (linked) {
                    loudest = max(loudest, levelDb);
                } else {
                    Detector& d = detector[c];
                    const double aim = curve(levelDb, t, r, w);
                    d.reduction = aim + (aim < d.reduction ? up : down) * (d.reduction - aim);
                }
            }
            if (linked) {
                Detector& d = detector[0];
                const double aim = curve(loudest, t, r, w);
                d.reduction = aim + (aim < d.reduction ? up : down) * (d.reduction - aim);
            }
            const double m = clamp(mix.at(i, 0), 0.0, 1.0);
            for (int c = 0; c < n; ++c) {
                const double cut = detector[linked ? 0 : c].reduction;
                deepest = min(deepest, cut);
                const double g = pow(10.0, (cut + makeup.at(i, c)) / 20.0);
                const double x = in[c][i];
                out[c][i] = m * g * x + (1.0 - m) * x;
                if (!linked || c == 0) gain[c][i] = pow(10.0, cut / 20.0);
            }
        }
        shownReduction.set(deepest);
    }

    void panel(Panel& p) {
        p.size(176, 344);
        p.display(8, 28, 160, 96);
        p.knob(threshold, 32, 158);
        p.knob(slope, 88, 158);
        p.knob(knee, 144, 158);
        p.knob(attack, 32, 214);
        p.knob(release, 88, 214);
        p.knob(makeup, 144, 214);
        p.knob(scFilter, 22, 262, 24);
        p.toggle(link, 44, 254, 36, 16);
        p.knob(mix, 110, 262, 24);
        p.input(in, 22, 318);
        p.input(sidechain, 66, 318);
        p.output(gain, 110, 318);
        p.output(out, 154, 318);
    }

    void draw(Canvas& g) {
        const double x0 = 8.0, y0 = 28.0, w = 160.0, h = 96.0;
        g.well(x0, y0, w, h);
        // The curve, -60 to 0 dB in and out, and a meter of the reduction.
        const double cx = x0 + 6.0, cy = y0 + 6.0, side = h - 12.0;
        g.strokeStyle(Theme::line);
        g.lineWidth(1.0);
        g.beginPath();
        g.moveTo(cx, cy + side);
        g.lineTo(cx + side, cy);
        g.stroke();
        g.strokeStyle(Theme::accent);
        g.lineWidth(1.6);
        g.beginPath();
        const double t = threshold.value(), r = max(slope.value(), 1.0),
                     kw = max(knee.value(), 0.0);
        for (int j = 0; j <= 60; ++j) {
            const double inDb = -60.0 + j;
            const double outDb = inDb + curve(inDb, t, r, kw);
            const double x = cx + side * j / 60.0;
            const double y = cy + side * clamp(-outDb / 60.0, 0.0, 1.0);
            if (j == 0)
                g.moveTo(x, y);
            else
                g.lineTo(x, y);
        }
        g.stroke();
        const double mx = x0 + w - 26.0, mh = side - 10.0;
        g.fillStyle(Theme::line);
        g.fillRect(mx, cy, 12.0, mh);
        const double cut = clamp(-shownReduction.get() / 24.0, 0.0, 1.0);
        g.fillStyle(Theme::accent);
        g.fillRect(mx, cy, 12.0, mh * cut);
        g.fillStyle(Theme::textFaint);
        g.font(8.0);
        g.textAlign(TextAlign::Center);
        g.fillText("GR", mx + 6.0, cy + mh + 9.0);
    }

    void factoryPatches(Patches& p) {
        p.add("Glue")
            .set(threshold, -20)
            .set(slope, 2)
            .set(attack, 30)
            .set(release, 200)
            .set(makeup, 0.5);
        p.add("Vocal")
            .set(threshold, -24)
            .set(slope, 4)
            .set(attack, 5)
            .set(release, 120)
            .set(makeup, 1.5);
        p.add("Drum Smash")
            .set(threshold, -30)
            .set(slope, 10)
            .set(attack, 1)
            .set(release, 60)
            .set(makeup, 6)
            .set(mix, 50);
        p.add("Duck")
            .set(threshold, -36)
            .set(slope, 8)
            .set(attack, 2)
            .set(release, 250)
            .set(scFilter, 60);
        p.add("Limit")
            .set(threshold, -6)
            .set(slope, 20)
            .set(knee, 2)
            .set(attack, 0.5)
            .set(release, 80);
    }
};
