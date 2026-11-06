// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A stereo delay, up to 10 seconds: echoes free in ms or in note values at the
// song's tempo. Feedback past 100 % builds instead of fading, held in check by
// a soft clip, so the repeats can swell and smear; Tone darkens (or thins)
// each repeat as it goes round. Wow and Flutter wobble the time as a tape
// echo's does, and moving Time glides the repeats' pitch the same way.
// Ping-pong sends the echoes from side to side. Send carries what goes round
// the loop; patch it through other modules into Return, and they play inside
// the feedback (a filter sweeping each repeat, a pitch shifter climbing).

constexpr double kMaxSeconds = 10.0;

// Linear to 0.5, then bending towards 1.2: repeats keep their level until
// they are loud.
double soften(double x) {
    const double a = abs(x);
    if (a <= 0.5) return x;
    const double over = (a - 0.5) / 0.7;
    const double y = 0.5 + 0.7 * over / sqrt(1.0 + over * over);
    return x < 0.0 ? -y : y;
}

// A note value's length in quarter notes, for Sync's options after Free.
double beatsOf(int option) {
    switch (option) {
        case 1:
            return 0.125;
        case 2:
            return 0.25 * 2.0 / 3.0;
        case 3:
            return 0.25;
        case 4:
            return 0.375;
        case 5:
            return 0.5 * 2.0 / 3.0;
        case 6:
            return 0.5;
        case 7:
            return 0.75;
        case 8:
            return 2.0 / 3.0;
        case 9:
            return 1.0;
        case 10:
            return 1.5;
        case 11:
            return 4.0 / 3.0;
        case 12:
            return 2.0;
        case 13:
            return 3.0;
        default:
            return 4.0;
    }
}

struct Side {
    DelayLine line;
    OnePole dark, thin, rumble;
    double time = 0.0;  // the delay now, in samples, gliding to its target
    double wet = 0.0;   // the last echo, toned
};

struct Delay : Module {
    static constexpr ModuleInfo info = {
        .name = "Delay",
        .id = "com.suppleaudio.rack.delay",
        .vendor = "Supple Audio",
        .description =
            "A stereo tape-style delay: synced or free, ping-pong, feedback past 100 %, "
            "and a loop to patch into.",
        .tags = {"delay", "effect", "stereo"}};

    Param sync = choice("Sync",
                        {"Free", "1/32", "1/16 T", "1/16", "1/16 D", "1/8 T", "1/8", "1/8 D",
                         "1/4 T", "1/4", "1/4 D", "1/2 T", "1/2", "1/2 D", "1/1"},
                        0)
                     .keys({"free", "n32", "n16t", "n16", "n16d", "n8t", "n8", "n8d", "n4t", "n4",
                            "n4d", "n2t", "n2", "n2d", "n1"})
                     .in("Time")
                     .help("The echo time in note values at the song's tempo, or Free for Time.");
    Param time = milliseconds("Time", 1, 10000, 375)
                     .in("Time")
                     .activeWhen(sync, {"Free"})
                     .help("The echo time when Sync is Free; moving it glides the pitch.");
    Param offset =
        percent("Offset", 0, -50, 50)
            .in("Time")
            .help("Makes the right side's time longer (or shorter) than the left's by this share.");
    Param feedback = percent("Feedback", 45, 0, 120)
                         .in("Echoes")
                         .help(
                             "How much of each echo goes round again; past 100 the repeats grow, "
                             "softly clipped.");
    Param tone = percent("Tone", -20, -100, 100)
                     .in("Echoes")
                     .help("Below 0 each repeat is darker, above 0 thinner; 0 leaves them clear.");
    Param pingPong =
        toggle("Ping Pong", false)
            .in("Echoes")
            .help("Echoes bounce from left to right: the input enters on the left only.");
    Param wow = percent("Wow", 15).in("Tape").help("A slow wobble of the time, as a tape's.");
    Param flutter =
        percent("Flutter", 10).in("Tape").help("A fast, slightly random shimmer of the time.");
    Param mix = percent("Mix", 25).smoothed(20).in("Output").help(
        "Dry to wet: below 50 the dry stays full, above it the dry fades. 25 puts the "
        "echoes 6 dB under the dry.");

    Input inL = audioIn("In L").help("The left input (poly cables are summed).");
    Input inR = audioIn("In R").help("The right input; the left feeds both while it is empty.");
    Input returnL = audioIn("Return L")
                        .in("Loop")
                        .help(
                            "Replaces the left feedback while connected: patch Send L through "
                            "something to it.");
    Input returnR = audioIn("Return R")
                        .in("Loop")
                        .help(
                            "Replaces the right feedback while connected; Return L feeds both "
                            "while it is empty.");

    Output outL = audioOut("Out L").help("The left output: dry and echoes.");
    Output outR = audioOut("Out R").help("The right output.");
    Output sendL = audioOut("Send L").in("Loop").help(
        "The left echoes as they go round again: to Return L through other modules.");
    Output sendR = audioOut("Send R").in("Loop").help("The right echoes as they go round again.");

    Array<Side, 2> side;
    Random random;
    double wowPhase = 0.0, flutterPhase = 0.0;
    double wowRate = 0.6, wobble = 0.0, wobbleTarget = 0.0;
    bool unplayed = true;

    void prepare() {
        const int most = toInt(kMaxSeconds * sampleRate()) + 64;
        for (int s = 0; s < 2; ++s) {
            side[s].line.prepare(most);
            side[s].rumble.setCutoff(25.0);
        }
        random.seed(seed());
        unplayed = true;
    }

    void process(int frames) {
        outL.setChannels(1);
        outR.setChannels(1);
        sendL.setChannels(1);
        sendR.setChannels(1);
        const double sr = sampleRate();
        const Transport t = transport();
        double target = time.value() * 0.001 * sr;
        if (sync.index() > 0) target = beatsOf(sync.index()) * 60.0 / max(t.tempo, 1.0) * sr;
        const double most = kMaxSeconds * sr;
        // Tone: below 0 a low pass falls from 20 kHz to 600 Hz, above 0 a high
        // pass rises from 20 Hz to 2 kHz.
        const double tn = tone.value();
        const double darkHz = tn < 0.0 ? 20000.0 * pow(0.03, -tn) : 20000.0;
        const double thinHz = tn > 0.0 ? 20.0 * pow(100.0, tn) : 20.0;
        for (int s = 0; s < 2; ++s) {
            side[s].dark.setCutoff(min(darkHz, 0.45 * sr));
            side[s].thin.setCutoff(thinHz);
        }
        // Wow's rate wanders between 0.4 and 0.9 Hz; flutter's amount
        // between half and all of itself, a new target every block.
        wowRate += (0.4 + 0.5 * random.next() - wowRate) * 0.01;
        wobbleTarget = 0.5 + 0.5 * random.next();
        const double glide = 1.0 - exp(-1.0 / (0.15 * sr));
        const bool cross = pingPong.on();
        for (int i = 0; i < frames; ++i) {
            wowPhase += wowRate / sr;
            wowPhase -= floor(wowPhase);
            flutterPhase += 8.7 / sr;
            flutterPhase -= floor(flutterPhase);
            wobble += (wobbleTarget - wobble) * 0.002;
            double l = 0.0, r = 0.0;
            for (int c = 0; c < max(inL.channels(), 1); ++c) l += inL[c][i];
            for (int c = 0; c < max(inR.channels(), 1); ++c) r += inR[c][i];
            if (!inR.connected()) r = l;
            const double timeNow = target;
            for (int s = 0; s < 2; ++s) {
                Side& d = side[s];
                const double skew = s == 1 ? 1.0 + offset.at(i, 0) : 1.0;
                const double aim = clamp(timeNow * skew, 1.0, most);
                d.time = unplayed ? aim : d.time + (aim - d.time) * glide;
                const double wowMs = 3.0 * wow.at(i, 0) * sin(twoPi * (wowPhase + 0.25 * s));
                const double flutterMs =
                    0.15 * flutter.at(i, 0) * wobble * sin(twoPi * (flutterPhase + 0.4 * s));
                const double at = clamp(d.time + (wowMs + flutterMs) * 0.001 * sr, 1.0, most);
                const double echo = d.line.read(at - 1.0);
                double toned = d.rumble.highpass(echo);
                if (tn < 0.0) toned = d.dark.lowpass(toned);
                if (tn > 0.0) toned = d.thin.highpass(toned);
                d.wet = toned;
            }
            unplayed = false;
            const double backL = returnL.connected() ? returnL[0][i] : side[0].wet;
            const double backR =
                returnR.connected() ? returnR[0][i] : (returnL.connected() ? backL : side[1].wet);
            const double fb = feedback.at(i, 0);
            const double intoL =
                cross ? 0.5 * (l + r) + soften(fb * backR) : l + soften(fb * backL);
            const double intoR = cross ? soften(fb * backL) : r + soften(fb * backR);
            side[0].line.push(intoL);
            side[1].line.push(intoR);
            const double m = mix.at(i, 0);
            const double dry = min(1.0, 2.0 - 2.0 * m), wet = min(1.0, 2.0 * m);
            outL[0][i] = dry * l + wet * side[0].wet;
            outR[0][i] = dry * r + wet * side[1].wet;
            sendL[0][i] = side[0].wet;
            sendR[0][i] = side[1].wet;
        }
    }
};
