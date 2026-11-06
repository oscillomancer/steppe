// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A looper for control voltages: it records a CV and a gate, a gesture on
// a knob, a played line, a filter sweep by hand, over a loop of one beat
// to eight bars locked to the song, and plays it back round and round.
// Record (or a high Rec) records; with Punch at Loop, recording starts
// and stops only at the loop's start, so a take is always whole loops, and
// a trigger at Rec records exactly the next loop. While it records, Out
// follows In; with nothing recorded yet, too. Depth and Offset scale and
// shift the playback (a recorded melody transposed, a sweep turned upside
// down). Changing Length stretches the loop to fit. With the song stopped
// it keeps going at the song's tempo; Reset starts the loop from its top,
// Clear forgets it. The recording is not saved with the patch.

constexpr int kLoopPoints = 8192;  // points a loop, whatever its length
constexpr int kLoopShown = 192;

struct Looper : Module {
    static constexpr ModuleInfo info = {
        .name = "Looper",
        .id = "com.suppleaudio.rack.looper",
        .vendor = "Supple Audio",
        .description = "Records a CV and a gate in time with the song and loops them.",
        .tags = {"utility", "sequencer", "modulation"}};

    Param length = choice("Length", {"1 Beat", "2 Beats", "1 Bar", "2 Bars", "4 Bars", "8 Bars"}, 2)
                       .keys({"beat_1", "beats_2", "bar_1", "bars_2", "bars_4", "bars_8"})
                       .help("How long the loop is, in the song's beats and bars.");
    Param punch = choice("Punch", {"Free", "Loop"}, 1)
                      .help(
                          "Free: records while Record is on. Loop: only whole loops, from the "
                          "loop's start; a trigger at Rec takes the next one.");
    Param record = toggle("Record", false).help("Records while on (and in time, at Punch Loop).");
    Param depth =
        percent("Depth", 100, -200, 200).help("Scales the playback: below 0 turns it upside down.");
    Param offset = continuous("Offset", -2, 2, 0).help("Added to the playback.");

    Input in = cvIn("In").help("The CV to record: any signal, a pitch, a knob's motion.");
    Input gate = gateIn("Gate").help("The gate to record beside it.");
    Input rec = gateIn("Rec").help(
        "Records while high, like Record; at Punch Loop a trigger records the next loop.");
    Input clear = gateIn("Clear").help("A rising edge forgets the recording.");
    Input reset = gateIn("Reset").help("A rising edge starts the loop from its top.");

    Output out = cvOut("Out").help("The loop, scaled by Depth and shifted by Offset.");
    Output outGate = gateOut("Gate").help("The recorded gate.");
    Output loop = gateOut("Loop").help("A trigger as the loop starts again.");

    Buffer<float> cv;
    Buffer<float> gates;
    bool filled = false;
    bool recording = false;
    bool wasRecording = false;
    bool armed = false;
    int lastIndex = -1;
    double freePhase = 0.0;  // beats into the loop, while the song is stopped
    double origin = 0.0;     // the song position the loop counts from
    double lastPos = -1.0;
    bool wasPlaying = false;
    bool dirty = true;
    Trigger gateEdge;
    Trigger recEdge;
    Trigger clearEdge;
    Trigger resetEdge;
    Pulse loopPulse;

    SharedArray<float, kLoopShown> shownLow;
    SharedArray<float, kLoopShown> shownHigh;
    SharedArray<int, kLoopShown> shownGate;
    Shared<float> shownPos;
    Shared<int> shownState;  // 0 playing or empty, 1 armed, 2 recording
    Shared<int> shownFilled;

    void factoryPatches(Patches& p) {
        p.add("One Bar Take").set(length, "bar_1").set(punch, "loop");
        p.add("Long Gesture").set(length, "bars_4").set(punch, "loop");
        p.add("Free Overwrite").set(length, "bars_2").set(punch, "free");
        p.add("Upside Down").set(length, "bar_1").set(depth, -100);
    }

    void prepare() {
        cv.resize(kLoopPoints);
        gates.resize(kLoopPoints);
        forget();
    }

    void forget() {
        cv.fill(0.0f);
        gates.fill(0.0f);
        filled = false;
        dirty = true;
    }

    double loopBeats(const Transport& t) const {
        const double bar = t.timeSigNumerator * 4.0 / max(1, t.timeSigDenominator);
        switch (length.index()) {
            case 0:
                return 1.0;
            case 1:
                return 2.0;
            case 2:
                return bar;
            case 3:
                return 2.0 * bar;
            case 4:
                return 4.0 * bar;
            default:
                return 8.0 * bar;
        }
    }

    void process(int frames) {
        out.setChannels(1);
        outGate.setChannels(1);
        loop.setChannels(1);
        const Transport t = transport();
        if (t.playing && !wasPlaying) origin = 0.0;
        wasPlaying = t.playing;
        const double beatsPerFrame = t.tempo / 60.0 / sampleRate();
        const double span = max(0.25, loopBeats(t));
        const bool quantized = punch.index() == 1;
        for (int i = 0; i < frames; ++i) {
            if (clearEdge.rose(clear[0][i])) forget();
            const bool resetNow = resetEdge.rose(reset[0][i]);
            const double songAt = t.beats + i * beatsPerFrame;
            if (resetNow) {
                origin = songAt;
                freePhase = 0.0;
            }
            double into = 0.0;  // beats into the loop
            if (t.playing) {
                into = songAt - origin;
                into = into - floor(into / span + 1e-9) * span;
            } else {
                into = freePhase;
                freePhase += beatsPerFrame;
                if (freePhase >= span) freePhase -= span;
            }
            const double pos = clamp(into / span, 0.0, 1.0);
            const bool wrapped = lastPos < 0.0 || pos < lastPos || resetNow;
            lastPos = pos;
            if (wrapped) loopPulse.fire();
            if (recEdge.rose(rec[0][i]) && quantized) armed = true;
            const bool want = record.on() || recEdge.high();
            if (!quantized) {
                recording = want;
                armed = false;
            } else if (wrapped) {
                recording = want || armed;
                armed = false;
            }
            const double x = in[0][i];
            const bool g = gateEdge.rose(gate[0][i]) || gateEdge.high();
            const int index = clamp(floorToInt(pos * kLoopPoints), 0, kLoopPoints - 1);
            if (recording) {
                // Every point passed since the last frame, so none is skipped
                // (a jump back, at a reset, writes only the point it lands on).
                int passed =
                    lastIndex >= 0 && wasRecording ? wrap(index - lastIndex, kLoopPoints) : 1;
                if (passed < 1 || passed > kLoopPoints / 2) passed = 1;
                for (int s = 0; s < passed; ++s) {
                    const int k = wrap(index - s, kLoopPoints);
                    cv[k] = static_cast<float>(x);
                    gates[k] = g ? 1.0f : 0.0f;
                }
                filled = true;
                dirty = true;
            }
            wasRecording = recording;
            lastIndex = index;
            double v = x;
            bool high = g;
            if (filled && !recording) {
                const double at = pos * kLoopPoints;
                const int k0 = clamp(floorToInt(at), 0, kLoopPoints - 1);
                const int k1 = wrap(k0 + 1, kLoopPoints);
                const double frac = at - k0;
                const double a = static_cast<double>(cv[k0]);
                const double b = static_cast<double>(cv[k1]);
                v = (a + (b - a) * frac) * depth.at(i) + offset.at(i);
                high = gates[k0] > 0.5f;
            }
            out[0][i] = v;
            outGate[0][i] = high ? 1.0 : 0.0;
            loop[0][i] = loopPulse.next();
        }
        if (dirty) {
            dirty = false;
            const int per = kLoopPoints / kLoopShown;
            for (int c = 0; c < kLoopShown; ++c) {
                double lo = static_cast<double>(cv[c * per]);
                double hi = lo;
                double on = 0.0;
                for (int k = c * per; k < (c + 1) * per; ++k) {
                    lo = min(lo, static_cast<double>(cv[k]));
                    hi = max(hi, static_cast<double>(cv[k]));
                    on = max(on, static_cast<double>(gates[k]));
                }
                shownLow[c] = static_cast<float>(lo);
                shownHigh[c] = static_cast<float>(hi);
                shownGate[c] = on > 0.5 ? 1 : 0;
            }
        }
        shownPos.set(static_cast<float>(max(0.0, lastPos)));
        shownState.set(recording ? 2 : (armed || (quantized && record.on()) ? 1 : 0));
        shownFilled.set(filled ? 1 : 0);
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(220, 344);
        p.display(8, 28, 204, 96);
        p.choice(length, 8, 132, 100, 18);
        p.choice(punch, 112, 132, 100, 18);
        p.toggle(record, 8, 156, 204, 18);
        p.knob(depth, 56, 206);
        p.knob(offset, 164, 206);
        p.input(in, 30, 260);
        p.input(gate, 83, 260);
        p.input(rec, 137, 260);
        p.input(clear, 190, 260);
        p.input(reset, 30, 314);
        p.output(out, 83, 314);
        p.output(outGate, 137, 314);
        p.output(loop, 190, 314);
    }

    // The loop's curve and its gates under it, with the playhead.
    void draw(Canvas& g) {
        const double left = 8.0;
        const double top = 28.0;
        const double w = 204.0;
        const double h = 96.0;
        g.well(left, top, w, h);
        const double x0 = left + 6.0;
        const double cw = (w - 12.0) / kLoopShown;
        const double curveTop = top + 8.0;
        const double curveH = h - 30.0;
        const double gateY = top + h - 14.0;
        const int state = shownState.get();
        if (shownFilled.get() == 1) {
            double peak = 1.0;
            for (int c = 0; c < kLoopShown; ++c) {
                peak = max(peak, abs(static_cast<double>(shownLow[c])));
                peak = max(peak, abs(static_cast<double>(shownHigh[c])));
            }
            const double mid = curveTop + curveH / 2.0;
            const double scaleY = curveH / 2.0 / peak;
            g.strokeStyle(Theme::line);
            g.lineWidth(1.0);
            g.beginPath();
            g.moveTo(x0, mid);
            g.lineTo(x0 + kLoopShown * cw, mid);
            g.stroke();
            g.fillStyle(Theme::accent);
            for (int c = 0; c < kLoopShown; ++c) {
                const double lo = static_cast<double>(shownLow[c]);
                const double hi = static_cast<double>(shownHigh[c]);
                g.fillRect(x0 + c * cw, mid - hi * scaleY - 0.75, cw + 0.3,
                           (hi - lo) * scaleY + 1.5);
            }
            g.fillStyle(Theme::accent2);
            for (int c = 0; c < kLoopShown; ++c)
                if (shownGate[c] == 1) g.fillRect(x0 + c * cw, gateY, cw + 0.3, 6.0);
        } else {
            g.fillStyle(Theme::textFaint);
            g.font(10.0);
            g.textAlign(TextAlign::Center);
            g.textBaseline(TextBaseline::Middle);
            g.fillText("empty: playing In through", left + w / 2.0, top + h / 2.0 - 6.0);
        }
        const double px = x0 + static_cast<double>(shownPos.get()) * kLoopShown * cw;
        g.strokeStyle(state == 2 ? Theme::accent : Theme::text);
        g.lineWidth(state == 2 ? 2.0 : 1.0);
        g.beginPath();
        g.moveTo(px, top + 4.0);
        g.lineTo(px, top + h - 4.0);
        g.stroke();
        if (state > 0) {
            g.fillStyle(Theme::accent);
            g.globalAlpha(state == 2 ? 1.0 : 0.5);
            g.beginPath();
            g.circle(left + w - 12.0, top + 11.0, 4.0);
            g.fill();
            g.globalAlpha(1.0);
        }
    }
};
