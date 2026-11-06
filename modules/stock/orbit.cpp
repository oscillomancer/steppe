// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A circular panner, poly: sends the input round a ring of four or eight
// speakers, one output each, clockwise from the front (1) at Rate turns a
// second, or the other way below 0. Angle sets where it starts or, at Rate
// 0, where it stands; Width spreads it from a point between two speakers
// to every speaker at once. The voices of a poly input sit Spread apart
// round the ring and turn together: a chord whose notes chase each other.
// L and R fold the ring down to stereo, each speaker at its place between
// them, for headphones or a pair of monitors.
//
// Doppler puts the sound on a circle half the ring's size inside a room up
// to 10 m across and delays each speaker's feed by its distance from it,
// so the pitch rises at the speakers it approaches and falls behind it as
// in a real room: a whirl at a few turns a second. The power over the
// speakers stays the same wherever the sound is.

constexpr int kVoices = 16;
constexpr double kSoundMps = 343.0;
constexpr double kMaxRoomM = 10.0;  // the ring's radius at Doppler 100
constexpr double kOrbit = 0.5;      // the sound's circle, a share of the ring's

struct Orbit : Module {
    static constexpr ModuleInfo info = {
        .name = "Orbit",
        .id = "com.suppleaudio.rack.orbit",
        .vendor = "Supple Audio",
        .description =
            "A circular panner: round four or eight speakers and a stereo pair, width, "
            "Doppler.",
        .tags = {"spatial", "panner", "poly"}};

    Param speakers = choice("Speakers", {"4 speakers", "8 speakers"}, 0)
                         .keys({"four", "eight"})
                         .help("The ring: four outputs (1 to 4) or eight.");
    Param rate = continuous("Rate", -10, 10, 0.2, Unit::Hertz)
                     .in("Motion")
                     .help("Turns a second: above 0 clockwise, below 0 the other way.");
    Param angle = degrees("Angle", -180, 180, 0)
                      .in("Motion")
                      .help("Where the sound is, added to the turning: 0 the front, 90 the right.");
    Param width = percent("Width", 0)
                      .in("Space")
                      .help("From a point between two speakers to every speaker at once.");
    Param spread = percent("Spread", 100)
                       .in("Space")
                       .help("How far apart a poly input's voices sit: 100 evenly round the ring.");
    Param doppler =
        percent("Doppler", 25)
            .smoothed(50)
            .in("Space")
            .help("The room's size, to 10 m across: the pitch bends as the sound moves.");

    Input in = audioIn("In").help("What to send round. Poly: each voice its own place.");
    Input reset = gateIn("Reset").help("Turns back to the front (and Angle) as it rises.");

    Output out1 = audioOut("1").help("The front speaker.");
    Output out2 = audioOut("2").help("The next speaker clockwise: the right, of four.");
    Output out3 = audioOut("3").help("The third speaker: behind, of four.");
    Output out4 = audioOut("4").help("The fourth speaker: the left, of four.");
    Output out5 = audioOut("5").help("The fifth speaker, of eight: behind.");
    Output out6 = audioOut("6").help("The sixth speaker, of eight.");
    Output out7 = audioOut("7").help("The seventh speaker, of eight: the left.");
    Output out8 = audioOut("8").help("The eighth speaker, of eight.");
    Output left = audioOut("L").help("The ring folded down to stereo: left.");
    Output right = audioOut("R").help("The ring folded down to stereo: right.");

    Array<DelayLine, kVoices> line;
    double turn = 0.0;  // the turning, in turns
    bool high = false;  // Reset, with hysteresis

    SharedArray<float, kVoices> shownAt;
    Shared<int> shownVoices;

    void prepare() {
        const double most = 2.0 * kMaxRoomM * (1.0 + kOrbit) / kSoundMps * sampleRate() + 8.0;
        for (int c = 0; c < kVoices; ++c) line[c].prepare(toInt(most));
    }

    void process(int frames) {
        const double sr = sampleRate();
        const int n = speakers.index() == 1 ? 8 : 4;
        const int voices = max(channels(), 1);
        out1.setChannels(1);
        out2.setChannels(1);
        out3.setChannels(1);
        out4.setChannels(1);
        out5.setChannels(1);
        out6.setChannels(1);
        out7.setChannels(1);
        out8.setChannels(1);
        left.setChannels(1);
        right.setChannels(1);
        Array<double, 8> feed;
        Array<double, 8> toLeft;
        Array<double, 8> toRight;
        for (int k = 0; k < n; ++k) {
            const double pan = sin(twoPi * k / n);
            toLeft[k] = sqrt(0.5 * (1.0 - pan));
            toRight[k] = sqrt(0.5 * (1.0 + pan));
        }
        for (int i = 0; i < frames; ++i) {
            const double r = reset[0][i];
            const bool rising = !high && r >= 0.5;
            high = high ? r >= 0.25 : r >= 0.5;
            if (rising) turn = 0.0;
            turn += clamp(rate.at(i, 0), -10.0, 10.0) / sr;
            turn -= floor(turn);
            const double w = clamp(width.at(i, 0), 0.0, 1.0);
            const double room = kMaxRoomM * clamp(doppler.at(i, 0), 0.0, 1.0);
            for (int k = 0; k < n; ++k) feed[k] = 0.0;
            for (int c = 0; c < voices; ++c) {
                line[c].push(in[c][i]);
                const double at =
                    turn + angle.at(i, c) / 360.0 + clamp(spread.at(i, c), 0.0, 1.0) * c / voices;
                const double place = (at - floor(at)) * n;
                const int s = min(floorToInt(place), n - 1);
                const double u = place - s;
                for (int k = 0; k < n; ++k) {
                    // Between the two speakers either side, sine and cosine;
                    // Width mixes in an equal share of every speaker, by
                    // power, so the total stays 1.
                    double v = 0.0;
                    if (k == s) v = cos(0.5 * pi * u);
                    if (k == (s + 1 == n ? 0 : s + 1)) v = sin(0.5 * pi * u);
                    const double g = sqrt((1.0 - w) * v * v + w / n);
                    // The sound on its circle, the speaker on the ring: the
                    // distance between, in the ring's radius, as a delay.
                    const double d =
                        sqrt(1.0 + kOrbit * kOrbit -
                             2.0 * kOrbit * cos(twoPi * (at - static_cast<double>(k) / n)));
                    feed[k] += g * line[c].read(room * d / kSoundMps * sr);
                }
                if (i == 0) shownAt[c] = static_cast<float>(at - floor(at));
            }
            double l = 0.0, rr = 0.0;
            for (int k = 0; k < n; ++k) {
                l += toLeft[k] * feed[k];
                rr += toRight[k] * feed[k];
            }
            out1[0][i] = feed[0];
            out2[0][i] = feed[1];
            out3[0][i] = feed[2];
            out4[0][i] = feed[3];
            out5[0][i] = n == 8 ? feed[4] : 0.0;
            out6[0][i] = n == 8 ? feed[5] : 0.0;
            out7[0][i] = n == 8 ? feed[6] : 0.0;
            out8[0][i] = n == 8 ? feed[7] : 0.0;
            left[0][i] = l;
            right[0][i] = rr;
        }
        shownVoices.set(in.connected() ? voices : 0);
    }

    void panel(Panel& p) {
        p.size(196, 414);
        p.display(8, 28, 180, 112);
        p.choice(speakers, 8, 146, 180, 18);
        p.knob(rate, 40, 198);
        p.knob(angle, 98, 198);
        p.knob(width, 156, 198);
        p.knob(spread, 69, 254);
        p.knob(doppler, 127, 254);
        p.input(in, 26, 300);
        p.input(reset, 26, 340);
        p.output(out1, 70, 300);
        p.output(out2, 102, 300);
        p.output(out3, 134, 300);
        p.output(out4, 166, 300);
        p.output(out5, 70, 340);
        p.output(out6, 102, 340);
        p.output(out7, 134, 340);
        p.output(out8, 166, 340);
        p.output(left, 134, 380);
        p.output(right, 166, 380);
    }

    void draw(Canvas& g) {
        const double x0 = 8.0, y0 = 28.0, w = 180.0, h = 112.0;
        g.well(x0, y0, w, h);
        const double cx = x0 + 0.5 * w, cy = y0 + 0.5 * h, radius = 0.5 * h - 14.0;
        const int n = speakers.index() == 1 ? 8 : 4;
        // The ring, the sound's circle and the speakers, numbered.
        g.strokeStyle(Theme::line);
        g.lineWidth(1.0);
        g.beginPath();
        g.circle(cx, cy, radius);
        g.stroke();
        g.beginPath();
        g.circle(cx, cy, kOrbit * radius);
        g.stroke();
        g.font(9.0);
        g.textAlign(TextAlign::Center);
        for (int k = 0; k < n; ++k) {
            const double a = twoPi * k / n;
            const double sx = cx + radius * sin(a), sy = cy - radius * cos(a);
            g.fillStyle(Theme::textFaint);
            g.beginPath();
            g.circle(sx, sy, 3.0);
            g.fill();
            g.fillStyle(Theme::text);
            g.fillText(format("%d", k + 1), cx + (radius + 9.0) * sin(a),
                       cy - (radius + 9.0) * cos(a) + 3.0);
        }
        // The voices where they were last sent; until something plays, one
        // where Angle puts it.
        int voices = shownVoices.get();
        g.fillStyle(Theme::accent);
        for (int c = 0; c < max(voices, 1); ++c) {
            const double at = voices > 0 ? static_cast<double>(shownAt[c]) : angle.value() / 360.0;
            const double a = twoPi * at;
            g.beginPath();
            g.circle(cx + kOrbit * radius * sin(a), cy - kOrbit * radius * cos(a), 4.0);
            g.fill();
        }
    }

    void factoryPatches(Patches& p) {
        p.add("Slow Circle").set(rate, 0.1);
        p.add("Whirl").set(rate, 3).set(doppler, 80);
        p.add("Wide Drift").set(rate, 0.05).set(width, 50);
        p.add("Octophonic").set(speakers, "eight").set(rate, 0.25);
        p.add("Still Left").set(rate, 0).set(angle, -90);
    }
};
