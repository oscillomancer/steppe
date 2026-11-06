// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A looping shift register of random bits. Each clock shifts the register
// along and feeds its last bit (at Length) back in at the front, flipping it
// first with the chance Flip: at 0 % the loop is locked and repeats every
// Length steps, a little lets it drift a bit at a time, 50 % is pure chance,
// and 100 % flips every bit, a loop twice as long that answers itself.
// Write and Clear force the bit coming in, to draw into the loop by hand.
// Out reads the first eight bits as a number (the newest the heaviest),
// scaled and offset: a melody that changes as slowly as Flip lets it,
// into a quantizer. Four gates follow bits 1, 2, 4 and 8, high with the
// clock: rhythms that move with the melody. With nothing in Clock it steps
// on the song's sixteenths; Reset starts the same register over.

struct Turing : Module {
    static constexpr ModuleInfo info = {
        .name = "Shift Register",
        .id = "com.suppleaudio.rack.turing",
        .vendor = "Supple Audio",
        .description = "A looping random shift register: locked, drifting or chaotic melodies.",
        .tags = {"random", "sequencer", "generative"}};

    Param flip = percent("Flip", 10)
                     .help(
                         "The chance the recycled bit flips: 0 % locks the loop, 50 % is pure "
                         "chance, 100 % a "
                         "loop twice as long.");
    Param length = integer("Length", 2, 16, 8).help("How many steps the loop has.");
    Param scale = octaves("Scale", 0, 4, 1).help("The span of Out, from all bits off to all on.");
    Param offset = octaves("Offset", -2, 2, 0).help("Added to Out: where its lowest value sits.");

    Input clock = gateIn("Clock").help(
        "Each rising edge shifts the register; with nothing here, the song's sixteenths.");
    Input reset = gateIn("Reset").help("A rising edge starts the same register over.");
    Input write = gateIn("Write").help("While high, the bit coming in is 1.");
    Input clear = gateIn("Clear").help("While high, the bit coming in is 0 (Write wins).");

    Output out = pitchOut("Out").help(
        "The first eight bits as a number, scaled and offset, in octaves: into a quantizer.");
    Output bit_1 = gateOut("Bit 1").help("High with the clock while the newest bit is set.");
    Output bit_2 = gateOut("Bit 2").help("High with the clock while bit 2 is set.");
    Output bit_4 = gateOut("Bit 4").help("High with the clock while bit 4 is set.");
    Output bit_8 = gateOut("Bit 8").help("High with the clock while bit 8 is set.");

    int bits = 0;  // the register, bit 0 the newest
    Random rng;
    Trigger clockEdge;
    Trigger resetEdge;
    bool clockHigh = false;  // the song's sixteenths, high for their first half
    bool wasPlaying = false;
    int salt = 0;

    SharedArray<int, 16> shownBits;
    SharedArray<float, 32> shownHistory;
    Shared<int> shownLength;

    void factoryPatches(Patches& p) {
        p.add("Slow Drift").set(flip, 5).set(length, 16);
        p.add("Locked Riff").set(flip, 0).set(length, 8);
        p.add("Call and Answer").set(flip, 100).set(length, 6);
        p.add("Chaos").set(flip, 50).set(scale, 2);
        p.add("Bass").set(flip, 8).set(length, 5).set(scale, 1).set(offset, -1);
    }

    void startOver() {
        rng.seed(salt);
        bits = 0;
        for (int k = 0; k < 16; ++k)
            if (rng.next() < 0.5) bits = bits | shl(1, k);
    }

    void prepare() {
        salt = seed();
        startOver();
        for (int k = 0; k < 32; ++k) shownHistory[k] = 0.0f;
    }

    double value() const {
        double v = 0.0;
        double weight = 0.5;
        for (int k = 0; k < 8; ++k) {
            v += bit(bits, k) * weight;
            weight *= 0.5;
        }
        return v / (1.0 - 1.0 / 256.0);
    }

    void shift(int i) {
        const int n = clamp(roundToInt(length.at(i)), 2, 16);
        // Every step draws once, whatever the knobs: a sequence depends only
        // on its seed and its clocks.
        const double draw = rng.next();
        int recycled = bit(bits, n - 1);
        if (draw < clamp(flip.at(i), 0.0, 1.0)) recycled = 1 - recycled;
        if (clear[0][i] >= 0.5) recycled = 0;
        if (write[0][i] >= 0.5) recycled = 1;
        bits = (shl(bits, 1) | recycled) & 65535;
        for (int k = 0; k < 31; ++k) shownHistory[k] = shownHistory[k + 1];
        shownHistory[31] = static_cast<float>(value());
    }

    void process(int frames) {
        out.setChannels(1);
        bit_1.setChannels(1);
        bit_2.setChannels(1);
        bit_4.setChannels(1);
        bit_8.setChannels(1);
        const Transport t = transport();
        const bool internal = !clock.connected();
        if (internal && t.playing && !wasPlaying) startOver();
        wasPlaying = t.playing;
        const double perFrame = t.tempo / 15.0 / sampleRate();
        for (int i = 0; i < frames; ++i) {
            if (resetEdge.rose(reset[0][i])) startOver();
            bool high = false;
            bool rose = false;
            if (internal) {
                if (t.playing) {
                    const double position = t.beats * 4.0 + i * perFrame + 1e-7;
                    high = position - floor(position) < 0.5;
                }
                rose = high && !clockHigh;
                clockHigh = high;
            } else {
                rose = clockEdge.rose(clock[0][i]);
                high = clockEdge.high();
            }
            if (rose) shift(i);
            out[0][i] = value() * scale.at(i) + offset.at(i);
            bit_1[0][i] = high && bit(bits, 0) == 1 ? 1.0 : 0.0;
            bit_2[0][i] = high && bit(bits, 1) == 1 ? 1.0 : 0.0;
            bit_4[0][i] = high && bit(bits, 3) == 1 ? 1.0 : 0.0;
            bit_8[0][i] = high && bit(bits, 7) == 1 ? 1.0 : 0.0;
        }
        for (int k = 0; k < 16; ++k) shownBits[k] = bit(bits, k);
        shownLength.set(clamp(length.index(), 2, 16));
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(220, 252);
        p.display(8, 28, 204, 66);
        p.knob(flip, 30, 124);
        p.knob(length, 83, 124);
        p.knob(scale, 137, 124);
        p.knob(offset, 190, 124);
        p.input(clock, 30, 180);
        p.input(reset, 83, 180);
        p.input(write, 137, 180);
        p.input(clear, 190, 180);
        p.output(out, 22, 224);
        p.output(bit_1, 66, 224);
        p.output(bit_2, 110, 224);
        p.output(bit_4, 154, 224);
        p.output(bit_8, 198, 224);
    }

    void draw(Canvas& g) {
        g.well(8, 28, 204, 66);
        const int n = clamp(shownLength.get(), 2, 16);
        const double cell = 192.0 / 16.0;
        // The register: the loop's cells bright, the rest faint.
        for (int k = 0; k < 16; ++k) {
            const double x = 14.0 + k * cell;
            g.globalAlpha(k < n ? 1.0 : 0.3);
            g.fillStyle(shownBits[k] == 1 ? Theme::accent : Theme::recess);
            g.beginPath();
            g.roundRect(x + 1, 33, cell - 2, 10, 2);
            g.fill();
            if (shownBits[k] != 1) {
                g.strokeStyle(Theme::lineLit);
                g.lineWidth(1.0);
                g.stroke();
            }
        }
        g.globalAlpha(1.0);
        // The last 32 values, oldest at the left.
        const double top = 50.0;
        const double h = 38.0;
        const double step = 192.0 / 32.0;
        g.strokeStyle(Theme::accent2);
        g.lineWidth(1.4);
        g.beginPath();
        for (int k = 0; k < 32; ++k) {
            const double v = clamp(static_cast<double>(shownHistory[k]), 0.0, 1.0);
            const double y = top + h * (1.0 - v);
            const double x = 14.0 + k * step;
            if (k == 0)
                g.moveTo(x, y);
            else
                g.lineTo(x, y);
            g.lineTo(x + step, y);
        }
        g.stroke();
    }
};
