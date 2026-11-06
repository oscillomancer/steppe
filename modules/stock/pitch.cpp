// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A pitch source and transposer: Octave, Semitone and Fine added to the
// pitch at In, channel by channel, so a whole poly cable moves together.
// With nothing at In it is a constant pitch, from C4 (all knobs at zero)
// up or down: a drone's note, or an oscillator's root. The display names
// the note going out (the first channel), its tuning in cents and its
// frequency.

struct Pitch : Module {
    static constexpr ModuleInfo info = {
        .name = "Pitch",
        .id = "com.suppleaudio.rack.pitch",
        .vendor = "Supple Audio",
        .description = "A constant pitch, or a transposer: octave, semitone and fine on a pitch.",
        .tags = {"pitch", "utility", "poly"}};

    Param octave = integer("Octave", -4, 4, 0).help("Whole octaves up or down.");
    Param semitone = integer("Semitone", -12, 12, 0).help("Semitones up or down.");
    Param fine = cents("Fine", -100, 100, 0).help("Cents up or down, to detune.");

    Input in = pitchIn("In").help(
        "The pitch to transpose, in octaves from C4; poly. With nothing here, C4.");

    Output out = pitchOut("Out").help("In, plus Octave, Semitone and Fine, in octaves from C4.");

    Shared<double> shown;  // the first channel's pitch, for the display

    void factoryPatches(Patches& p) {
        p.add("C4");
        p.add("A4").set(semitone, 9);
        p.add("Octave Down").set(octave, -1);
        p.add("Fifth Up").set(semitone, 7);
        p.add("Detune").set(fine, 7);
    }

    void process(int frames) {
        const int n = max(1, in.channels());
        out.setChannels(n);
        for (int i = 0; i < frames; ++i)
            for (int c = 0; c < n; ++c)
                out[c][i] = in[c][i] + roundToInt(octave.at(i, c)) +
                            roundToInt(semitone.at(i, c)) / 12.0 + fine.at(i, c) / 1200.0;
        shown.set(out[0][frames - 1]);
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(120, 318);
        p.display(8, 28, 104, 66);
        p.knob(octave, 60, 128);
        p.knob(semitone, 60, 182);
        p.knob(fine, 60, 236);
        p.input(in, 32, 290);
        p.output(out, 88, 290);
    }

    void noteName(Canvas& g, int pc, double x, double y) {
        switch (pc) {
            case 0:
                g.fillText("C", x, y);
                break;
            case 1:
                g.fillText("C#", x, y);
                break;
            case 2:
                g.fillText("D", x, y);
                break;
            case 3:
                g.fillText("D#", x, y);
                break;
            case 4:
                g.fillText("E", x, y);
                break;
            case 5:
                g.fillText("F", x, y);
                break;
            case 6:
                g.fillText("F#", x, y);
                break;
            case 7:
                g.fillText("G", x, y);
                break;
            case 8:
                g.fillText("G#", x, y);
                break;
            case 9:
                g.fillText("A", x, y);
                break;
            case 10:
                g.fillText("A#", x, y);
                break;
            default:
                g.fillText("B", x, y);
                break;
        }
    }

    void draw(Canvas& g) {
        g.well(8, 28, 104, 66);
        const double v = clamp(shown.get(), -10.0, 10.0);
        const int note = roundToInt(v * 12.0);
        const int pc = imod(note, 12);
        const int oct = floorToInt(note / 12.0) + 4;
        const double off = (v * 12.0 - note) * 100.0;  // cents from the note
        // The note and its octave.
        g.fillStyle(Theme::text);
        g.boldFont(20);
        g.textAlign(TextAlign::Right);
        g.textBaseline(TextBaseline::Alphabetic);
        noteName(g, pc, 66, 58);
        g.fillStyle(Theme::textMuted);
        g.font(13);
        g.textAlign(TextAlign::Left);
        g.fillText(format("%d", oct), 68, 58);
        // The tuning: a dot on a scale of a semitone, the note in the middle.
        const double left = 18.0;
        const double width = 84.0;
        const double y = 70.0;
        g.strokeStyle(Theme::lineLit);
        g.lineWidth(1.0);
        g.beginPath();
        g.moveTo(left, y);
        g.lineTo(left + width, y);
        for (int k = -2; k <= 2; ++k) {
            const double x = left + width * (0.5 + k * 0.25);
            const double tick = k == 0 ? 4.0 : 2.0;
            g.moveTo(x, y - tick);
            g.lineTo(x, y + tick);
        }
        g.stroke();
        g.fillStyle(abs(off) < 1.0 ? Theme::accent : Theme::accent2);
        g.beginPath();
        g.circle(left + width * (0.5 + off / 100.0), y, 3.2);
        g.fill();
        // Cents and hertz.
        g.monoFont(9);
        g.fillStyle(Theme::textFaint);
        g.textBaseline(TextBaseline::Bottom);
        g.textAlign(TextAlign::Left);
        g.fillText(format("%+.0f c", off), 14, 90);
        g.textAlign(TextAlign::Right);
        g.fillText(format("%.1f Hz", 261.6256 * pow(2.0, v)), 106, 90);
    }
};
