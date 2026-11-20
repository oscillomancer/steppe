// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Gain: a level change and nothing else, at the point in a chain where a
// fader cannot reach: to drive the next plugin harder, or trim after one.
//
// The level glides (the framework's 20 ms smoothing, in dB), so moving or
// automating it never clicks; a glide starts at the sample its change
// arrives, and plays the same in blocks of any size. At 0 dB the input
// passes as it came, bit for bit.
//
// The editor is the knob and its value: drag it, or anywhere in the
// window, up and down (with Shift, finer); the wheel turns it and a double
// click returns it to 0 dB.

// --- the look --------------------------------------------------------------------------

constexpr int kInk = 0x1A1411;
constexpr int kGroundTop = 0x2B221B;
constexpr int kGroundBottom = 0x201914;
constexpr int kKnobLight = 0x54463B;
constexpr int kKnobHot = 0x5E4E41;
constexpr int kKnobDark = 0x2A221C;
constexpr int kClay = 0xD49A5B;

constexpr double kW = 180;
constexpr double kH = 186;
constexpr double kX = 90, kY = 84, kR = 54;  // the knob

struct Gain : Effect {
    static constexpr Info info = {
        .name = "Gain",
        .id = "com.suppleaudio.gain",
        .vendor = "Supple Audio",
        .version = "1.0.0",
        .description =
            "A level change of up to 32 dB either way, smoothed so that moving it "
            "never clicks.",
        .features = {"utility"}};

    Param gain = decibels("Gain", -32, 32, 0)
                     .modulated()
                     .smoothed(20)
                     .help("The level change, in dB; it glides there, so moving it never clicks.");

    // --- audio ------------------------------------------------------------------------------

    double gainDb = 0.0, gainLinear = 1.0;

    void process(Block& io) {
        for (int i = 0; i < io.frames; ++i) {
            const double db = gain.at(i);
            // Reuse the exact conversion once the smoothed gain holds still.
            if (db != gainDb) {
                gainDb = db;
                gainLinear = dbToGain(db);
            }
            // At 0 dB the input passes as it came, bit for bit.
            if (gainLinear != 1.0)
                for (int c = 0; c < io.channels; ++c) io[c][i] *= gainLinear;
        }
    }

    // --- the editor -------------------------------------------------------------------------

    bool dragging = false;
    bool hot = false;
    double knobPos = 0.5;

    void editor(Editor& e) { e.size(kW, kH); }

    void draw(Canvas& g) {
        g.linearGradient(0, 0, 0, kH, kGroundTop, kGroundBottom);
        g.fillRect(0, 0, kW, kH);

        // The track from 7 to 5 o'clock, lit from 0 dB at the top to the value.
        const double start = 0.75 * pi, sweep = 1.5 * pi;
        const double at = start + sweep * clamp(gain.normalized(), 0.0, 1.0);
        const double base = start + sweep * 0.5;
        g.lineCap(LineCap::Round);
        g.strokeStyle(kInk);
        g.lineWidth(8);
        g.beginPath();
        g.arc(kX, kY, kR, start, start + sweep);
        g.stroke();
        g.strokeStyle(kClay);
        g.lineWidth(6);
        if (abs(at - base) > 0.001) {
            g.beginPath();
            if (at >= base)
                g.arc(kX, kY, kR, base, at);
            else
                g.arc(kX, kY, kR, at, base);
            g.stroke();
        }
        // 0 dB: a notch outside the track.
        g.strokeStyle(Theme::textFaint);
        g.lineWidth(1.5);
        g.beginPath();
        g.moveTo(kX, kY - kR - 7);
        g.lineTo(kX, kY - kR - 11);
        g.stroke();

        // The cap, and its pointer.
        g.radialGradient(kX, kY - kR * 0.25, 2, kR - 6, hot || dragging ? kKnobHot : kKnobLight,
                         kKnobDark);
        g.beginPath();
        g.circle(kX, kY, kR - 10);
        g.fill();
        g.strokeStyle(Theme::line);
        g.lineWidth(1);
        g.stroke();
        g.strokeStyle(Theme::text);
        g.lineWidth(2.5);
        g.beginPath();
        g.moveTo(kX + cos(at) * (kR - 26), kY + sin(at) * (kR - 26));
        g.lineTo(kX + cos(at) * (kR - 15), kY + sin(at) * (kR - 15));
        g.stroke();

        // The value, and its unit smaller, centred together under the knob.
        const double db = gain.value();
        const bool zero = abs(db) < 0.05;
        g.monoFont(18);
        const double nw = zero ? g.measureText("0.0") : g.measureText(format("%+.1f", db));
        g.monoFont(11);
        const double left = kX - (nw + 4 + g.measureText("dB")) * 0.5;
        const double baseline = kY + kR + 28;
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Alphabetic);
        g.fillStyle(Theme::text);
        g.monoFont(18);
        if (zero)
            g.fillText("0.0", left, baseline);
        else
            g.fillText(format("%+.1f", db), left, baseline);
        g.fillStyle(Theme::textMuted);
        g.monoFont(11);
        g.fillText("dB", left + nw + 4, baseline);
    }

    // --- the pointer ------------------------------------------------------------------------

    // The whole window is the knob's: there is nothing else to touch.
    void pointer(const Pointer& p) {
        const PointerAction a = p.action();
        const double fine = p.shift() ? 0.2 : 1.0;
        if (a == PointerAction::Leave) {
            hot = false;
            return;
        }
        if (a == PointerAction::Move) {
            hot = p.within(kX, kY, kR + 6);
            return;
        }
        if (a == PointerAction::Press) {
            if (p.doubleClick()) {
                gain.reset();
                return;
            }
            dragging = true;
            knobPos = gain.normalized();
            return;
        }
        if (a == PointerAction::Drag) {
            if (dragging) {
                knobPos = clamp(knobPos - p.dy() * fine / 200.0, 0.0, 1.0);
                gain.editNormalized(knobPos);
            }
            return;
        }
        if (a == PointerAction::Release) {
            dragging = false;
            hot = p.within(kX, kY, kR + 6);
            return;
        }
        // Half a dB a notch.
        if (a == PointerAction::Wheel)
            gain.edit(clamp(gain.value() + p.wheel() * 0.5 * fine, -32.0, 32.0));
    }
};
