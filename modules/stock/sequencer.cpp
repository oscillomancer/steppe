// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A 16-step sequencer. Each step has a note (semitones), a gate, a length
// (100 % ties it into the next step, legato) and a chance to play. Steps
// sets the pattern's length and Direction its walk: forward, backward,
// ping-pong, random or drunk (a step either way). Each clock plays the next
// step; with nothing in Clock it steps on the song's sixteenth notes while
// the transport plays, locked to the song position. Reset makes the next
// clock play the first step. Pitch in Transpose is added to every note: a
// poly cable (a chord) sequences every voice of it. Random walks and chances
// come from the step's count since the reset, so a pattern plays the same
// every time it starts. On its face: click a step's gate to toggle it (drag
// to paint), drag in the lanes to set pitches, lengths and chances.

// A random 0..1 for step count n: the same wherever that step plays.
double stepRandom(int n, int salt) {
    int h = n * 668265261 + salt * 374761393 + 1013904223;
    h = h ^ ((h >> 15) & 131071);
    h = h * 739982445;
    h = h ^ ((h >> 12) & 1048575);
    h = h * 695872825;
    h = h ^ ((h >> 15) & 131071);
    return static_cast<double>((h >> 8) & 65535) / 65536.0;
}

constexpr int kSteps = 16;

struct Sequencer : Module {
    static constexpr ModuleInfo info = {
        .name = "Sequencer",
        .id = "com.suppleaudio.rack.sequencer",
        .vendor = "Supple Audio",
        .description = "Sixteen steps of notes, gates, lengths and chances, with five walks.",
        .tags = {"sequencer", "generative"}};

    Param steps = integer("Steps", 1, 16, 16)
                      .in("Pattern")
                      .help("How many steps the pattern plays, from the first.");
    Param direction =
        choice("Direction", {"Forward", "Backward", "Ping-Pong", "Random", "Drunk"}, 0)
            .in("Pattern")
            .help(
                "How it walks the steps: forward, backward, to and fro, at "
                "random, or drunk (one step either way at random).");
    Param pitch_1 = semitones("Pitch 1", -24, 24, 0)
                        .in("Pitches")
                        .help("Step 1's note in semitones, added to Transpose (0 is C4).");
    Param pitch_2 = semitones("Pitch 2", -24, 24, 12)
                        .in("Pitches")
                        .help("Step 2's note in semitones, added to Transpose (0 is C4).");
    Param pitch_3 = semitones("Pitch 3", -24, 24, 7)
                        .in("Pitches")
                        .help("Step 3's note in semitones, added to Transpose (0 is C4).");
    Param pitch_4 = semitones("Pitch 4", -24, 24, 0)
                        .in("Pitches")
                        .help("Step 4's note in semitones, added to Transpose (0 is C4).");
    Param pitch_5 = semitones("Pitch 5", -24, 24, 3)
                        .in("Pitches")
                        .help("Step 5's note in semitones, added to Transpose (0 is C4).");
    Param pitch_6 = semitones("Pitch 6", -24, 24, 12)
                        .in("Pitches")
                        .help("Step 6's note in semitones, added to Transpose (0 is C4).");
    Param pitch_7 = semitones("Pitch 7", -24, 24, 10)
                        .in("Pitches")
                        .help("Step 7's note in semitones, added to Transpose (0 is C4).");
    Param pitch_8 = semitones("Pitch 8", -24, 24, 7)
                        .in("Pitches")
                        .help("Step 8's note in semitones, added to Transpose (0 is C4).");
    Param pitch_9 = semitones("Pitch 9", -24, 24, 0)
                        .in("Pitches")
                        .help("Step 9's note in semitones, added to Transpose (0 is C4).");
    Param pitch_10 = semitones("Pitch 10", -24, 24, 12)
                         .in("Pitches")
                         .help("Step 10's note in semitones, added to Transpose (0 is C4).");
    Param pitch_11 = semitones("Pitch 11", -24, 24, 7)
                         .in("Pitches")
                         .help("Step 11's note in semitones, added to Transpose (0 is C4).");
    Param pitch_12 = semitones("Pitch 12", -24, 24, 0)
                         .in("Pitches")
                         .help("Step 12's note in semitones, added to Transpose (0 is C4).");
    Param pitch_13 = semitones("Pitch 13", -24, 24, 5)
                         .in("Pitches")
                         .help("Step 13's note in semitones, added to Transpose (0 is C4).");
    Param pitch_14 = semitones("Pitch 14", -24, 24, 3)
                         .in("Pitches")
                         .help("Step 14's note in semitones, added to Transpose (0 is C4).");
    Param pitch_15 = semitones("Pitch 15", -24, 24, 10)
                         .in("Pitches")
                         .help("Step 15's note in semitones, added to Transpose (0 is C4).");
    Param pitch_16 = semitones("Pitch 16", -24, 24, 12)
                         .in("Pitches")
                         .help("Step 16's note in semitones, added to Transpose (0 is C4).");
    Params<16> gates =
        toggle("Gate", true).withKey("gate").in("Gates").help("Whether the step plays a note.");
    Params<16> lengths = percent("Length", 50, 5, 100)
                             .withKey("length")
                             .in("Lengths")
                             .help(
                                 "How long the step's gate is, as a share of the step; 100 % ties "
                                 "it into the next.");
    Params<16> chances = percent("Chance", 100)
                             .withKey("chance")
                             .in("Chances")
                             .help("How likely the step is to play when its gate is on.");

    Input clock = gateIn("Clock").help(
        "Each rising edge plays the next step; with nothing here, the song's sixteenths.");
    Input reset = gateIn("Reset").help("A rising edge makes the next clock play the first step.");
    Input transpose =
        pitchIn("Transpose")
            .help(
                "Added to every note, in octaves; a poly cable plays the pattern in every voice.");

    Output pitch = pitchOut("Pitch").help("The note of the step playing, in octaves from C4.");
    Output gate = gateOut("Gate").help("High for each played step's length.");
    Output step = gateOut("Step").help("A 1 ms trigger at every step, played or not.");
    Output end = gateOut("End").help("A 1 ms trigger as the pattern comes round to its start.");
    Output position =
        unipolarOut("Position").help("Where the pattern is: 0 at the first step, 1 at the last.");

    Array<double, 16> stepPitch;
    Array<bool, 16> stepGate;
    Array<double, 16> stepLength;
    Array<double, 16> stepChance;

    Trigger clockEdge;
    Trigger resetEdge;
    bool armed = false;  // the next clock plays the first step
    int count = -1;      // steps played since the reset
    int base = 0;        // the sixteenth a reset started from (internal clock)
    int lastSixteenth = -2147483647;
    int current = 0;        // the step playing
    int sounding = 0;       // the last step that played a note: its pitch is out
    int walk = 0;           // the drunk walk's step
    bool playing = false;   // whether the current step plays a note
    int gateLeft = 0;       // frames of gate left; -1 until the next step
    bool lowFrame = false;  // one frame low, so a new note retriggers
    bool gateWasHigh = false;
    int sinceClock = 0;
    int period = 6000;
    bool clocked = false;
    Pulse stepPulse;
    Pulse endPulse;
    int salt = 0;

    Shared<int> shownStep;
    Shared<bool> shownPlaying;

    // The editor's own: which lane a drag sets, and what it is doing.
    int lane = 0;
    int dragMode = 0;  // 0 none, 1 a lane's values, 2 painting gates
    bool paintOn = true;
    int lastPainted = -1;

    // --- the steps by number ------------------------------------------------------------
    // Every step's settings at the block's start, with the cables into them.
    void readSteps() {
        stepPitch[0] = pitch_1.at(0);
        stepPitch[1] = pitch_2.at(0);
        stepPitch[2] = pitch_3.at(0);
        stepPitch[3] = pitch_4.at(0);
        stepPitch[4] = pitch_5.at(0);
        stepPitch[5] = pitch_6.at(0);
        stepPitch[6] = pitch_7.at(0);
        stepPitch[7] = pitch_8.at(0);
        stepPitch[8] = pitch_9.at(0);
        stepPitch[9] = pitch_10.at(0);
        stepPitch[10] = pitch_11.at(0);
        stepPitch[11] = pitch_12.at(0);
        stepPitch[12] = pitch_13.at(0);
        stepPitch[13] = pitch_14.at(0);
        stepPitch[14] = pitch_15.at(0);
        stepPitch[15] = pitch_16.at(0);
        for (int k = 0; k < kSteps; ++k) {
            stepGate[k] = gates[k].on();
            stepLength[k] = lengths[k].at(0);
            stepChance[k] = chances[k].at(0);
        }
    }
    double pitchShown(int k) const {
        switch (k) {
            case 0:
                return pitch_1.value();
            case 1:
                return pitch_2.value();
            case 2:
                return pitch_3.value();
            case 3:
                return pitch_4.value();
            case 4:
                return pitch_5.value();
            case 5:
                return pitch_6.value();
            case 6:
                return pitch_7.value();
            case 7:
                return pitch_8.value();
            case 8:
                return pitch_9.value();
            case 9:
                return pitch_10.value();
            case 10:
                return pitch_11.value();
            case 11:
                return pitch_12.value();
            case 12:
                return pitch_13.value();
            case 13:
                return pitch_14.value();
            case 14:
                return pitch_15.value();
            case 15:
                return pitch_16.value();
            default:
                return 0.0;
        }
    }
    void editPitch(int k, double v) const {
        switch (k) {
            case 0:
                pitch_1.edit(v);
                break;
            case 1:
                pitch_2.edit(v);
                break;
            case 2:
                pitch_3.edit(v);
                break;
            case 3:
                pitch_4.edit(v);
                break;
            case 4:
                pitch_5.edit(v);
                break;
            case 5:
                pitch_6.edit(v);
                break;
            case 6:
                pitch_7.edit(v);
                break;
            case 7:
                pitch_8.edit(v);
                break;
            case 8:
                pitch_9.edit(v);
                break;
            case 9:
                pitch_10.edit(v);
                break;
            case 10:
                pitch_11.edit(v);
                break;
            case 11:
                pitch_12.edit(v);
                break;
            case 12:
                pitch_13.edit(v);
                break;
            case 13:
                pitch_14.edit(v);
                break;
            case 14:
                pitch_15.edit(v);
                break;
            case 15:
                pitch_16.edit(v);
                break;
            default:
                break;
        }
    }
    // --- playing ---------------------------------------------------------------------------

    void factoryPatches(Patches& p) {
        p.add("Rising Arp");
        p.add("Flat")
            .set(pitch_2, 0)
            .set(pitch_3, 0)
            .set(pitch_5, 0)
            .set(pitch_6, 0)
            .set(pitch_7, 0)
            .set(pitch_8, 0)
            .set(pitch_10, 0)
            .set(pitch_11, 0)
            .set(pitch_13, 0)
            .set(pitch_14, 0)
            .set(pitch_15, 0)
            .set(pitch_16, 0);
        p.add("Acid Line")
            .set(pitch_1, 0)
            .set(pitch_2, 0)
            .set(pitch_3, 12)
            .set(pitch_4, 0)
            .set(pitch_5, 3)
            .set(pitch_6, 0)
            .set(pitch_7, 15)
            .set(pitch_8, 12)
            .set(pitch_9, 0)
            .set(pitch_10, 10)
            .set(pitch_11, 0)
            .set(pitch_12, 12)
            .set(pitch_13, 0)
            .set(pitch_14, 7)
            .set(pitch_15, 5)
            .set(pitch_16, 3)
            .set(lengths[1], 100)
            .set(lengths[6], 100)
            .set(lengths[10], 100)
            .set(lengths[14], 100)
            .set(gates[3], false)
            .set(gates[8], false)
            .set(gates[12], false);
        p.add("Sparse Bells")
            .set(direction, "random")
            .set(chances[0], 60)
            .set(chances[1], 25)
            .set(chances[2], 40)
            .set(chances[3], 25)
            .set(chances[4], 50)
            .set(chances[5], 25)
            .set(chances[6], 40)
            .set(chances[7], 25)
            .set(chances[8], 60)
            .set(chances[9], 25)
            .set(chances[10], 40)
            .set(chances[11], 25)
            .set(chances[12], 50)
            .set(chances[13], 25)
            .set(chances[14], 40)
            .set(chances[15], 25);
        p.add("Eight Drunk Steps").set(steps, 8).set(direction, "drunk");
    }

    void prepare() { salt = seed(); }

    // The step for count n of a pattern len long.
    int stepFor(int n, int len, int mode) {
        if (mode == 1) return len - 1 - wrap(n, len);
        if (mode == 2) {
            const int cycle = 2 * (len - 1);
            if (cycle <= 0) return 0;
            const int k = wrap(n, cycle);
            return k < len ? k : cycle - k;
        }
        if (mode == 3) return min(len - 1, floorToInt(stepRandom(n, salt) * len));
        if (mode == 4) {
            if (n <= 0) {
                walk = 0;
            } else {
                walk = wrap(walk + (stepRandom(n, salt) < 0.5 ? -1 : 1), len);
            }
            return walk;
        }
        return wrap(n, len);
    }

    // A clock: the next step (or the first, after a reset).
    void advance(int sixteenth, bool internal) {
        if (clocked && !internal) period = max(1, sinceClock);
        if (internal)
            period = max(1, roundToInt(sampleRate() * 15.0 / max(1.0, transport().tempo)));
        clocked = true;
        sinceClock = 0;
        if (armed) {
            count = 0;
            base = sixteenth;
            armed = false;
        } else {
            count = internal ? sixteenth - base : count + 1;
        }
        const int len = clamp(roundToInt(steps.at(0)), 1, kSteps);
        const int mode = clamp(direction.index(), 0, 4);
        const int next = stepFor(count, len, mode);
        const bool tied = playing && stepLength[current] >= 0.999;
        const bool plays = stepGate[next] && stepRandom(count, salt + 101) < stepChance[next];
        current = next;
        playing = plays;
        stepPulse.fire();
        const int cycle = mode == 2 ? max(1, 2 * (len - 1)) : len;
        if (count > 0 && wrap(count, cycle) == 0) endPulse.fire();
        if (!plays) {
            gateLeft = 0;
            return;
        }
        sounding = next;
        lowFrame = gateWasHigh && !tied;
        gateLeft = stepLength[next] >= 0.999 ? -1 : max(1, roundToInt(stepLength[next] * period));
    }

    void process(int frames) {
        readSteps();
        const Transport t = transport();
        const bool internal = !clock.connected();
        const double sixteenthsPerFrame = t.tempo / 15.0 / sampleRate();
        const int len = clamp(roundToInt(steps.at(0)), 1, kSteps);
        gate.setChannels(1);
        step.setChannels(1);
        end.setChannels(1);
        position.setChannels(1);
        for (int i = 0; i < frames; ++i) {
            if (resetEdge.rose(reset[0][i])) armed = true;
            if (internal) {
                if (t.playing) {
                    const int s = floorToInt(t.beats * 4.0 + i * sixteenthsPerFrame + 1e-9);
                    if (s != lastSixteenth) advance(s, true);
                    lastSixteenth = s;
                } else {
                    lastSixteenth = -2147483647;
                }
            } else {
                if (clockEdge.rose(clock[0][i])) advance(0, false);
            }
            ++sinceClock;

            bool high = false;
            if (lowFrame) {
                lowFrame = false;
            } else if (gateLeft != 0) {
                high = true;
                if (gateLeft > 0) --gateLeft;
            }
            gate[0][i] = high ? 1.0 : 0.0;
            gateWasHigh = high;
            step[0][i] = stepPulse.next();
            end[0][i] = endPulse.next();
            position[0][i] = len > 1 ? static_cast<double>(min(current, len - 1)) / (len - 1) : 0.0;
            for (int c = 0; c < channels(); ++c)
                pitch[c][i] = stepPitch[sounding] / 12.0 + transpose[c][i];
        }
        shownStep.set(current);
        shownPlaying.set(gateLeft != 0);
    }

    // --- the face ----------------------------------------------------------------------------

    static constexpr double kGridX = 12.0;
    static constexpr double kGridW = 240.0;
    static constexpr double kTabsY = 31.0;
    static constexpr double kLaneTop = 50.0;
    static constexpr double kLaneH = 74.0;
    static constexpr double kGateY = 130.0;
    static constexpr double kGateH = 16.0;

    void panel(Panel& p) {
        p.size(264, 300);
        p.display(8, 28, 248, 124);
        p.knob(steps, 30, 184);
        p.choice(direction, 64, 175, 192, 18);
        p.input(clock, 30, 232);
        p.input(reset, 74, 232);
        p.input(transpose, 118, 232);
        p.output(pitch, 58, 272);
        p.output(gate, 102, 272);
        p.output(step, 146, 272);
        p.output(end, 190, 272);
        p.output(position, 234, 272);
    }

    double column(double x) const { return (x - kGridX) / (kGridW / kSteps); }

    void draw(Canvas& g) {
        g.well(8, 28, 248, 124);
        const int len = clamp(steps.index(), 1, kSteps);
        const int playingStep = shownStep.get();
        const double colW = kGridW / kSteps;

        // The lane tabs.
        g.font(9);
        g.textBaseline(TextBaseline::Middle);
        g.textAlign(TextAlign::Center);
        for (int k = 0; k < 3; ++k) {
            const double cx = kGridX + 40.0 + k * 80.0;
            g.fillStyle(k == lane ? Theme::accent : Theme::textMuted);
            g.fillText(k == 0 ? "PITCH" : (k == 1 ? "LENGTH" : "CHANCE"), cx, kTabsY + 6);
            if (k == lane) g.fillRect(cx - 18, kTabsY + 13, 36, 1.5);
        }

        const double bottom = kLaneTop + kLaneH;
        if (lane == 0) {
            // C4 and the octaves either side.
            g.strokeStyle(Theme::line);
            g.lineWidth(1.0);
            g.beginPath();
            for (int o = 1; o < 4; ++o) {
                g.moveTo(kGridX, kLaneTop + kLaneH * o * 0.25);
                g.lineTo(kGridX + kGridW, kLaneTop + kLaneH * o * 0.25);
            }
            g.stroke();
        }
        for (int k = 0; k < kSteps; ++k) {
            const double x = kGridX + k * colW;
            g.globalAlpha(k < len ? 1.0 : 0.3);
            if (k == playingStep) {
                g.fillStyle(Theme::raised);
                g.fillRect(x, kLaneTop - 2, colW, kLaneH + kGateH + 10);
            }
            g.fillStyle(Theme::accent);
            if (lane == 0) {
                const double y = kLaneTop + kLaneH * (1.0 - (pitchShown(k) + 24.0) / 48.0);
                const double mid = kLaneTop + kLaneH * 0.5;
                g.globalAlpha((k < len ? 1.0 : 0.3) * (gates[k].on() ? 0.35 : 0.15));
                g.fillRect(x + 2, min(y, mid), colW - 4, abs(mid - y));
                g.globalAlpha(k < len ? 1.0 : 0.3);
                g.fillRect(x + 2, y - 1.5, colW - 4, 3);
            } else {
                const double v = lane == 1 ? lengths[k].value() : chances[k].value();
                const double y = bottom - kLaneH * clamp(v, 0.0, 1.0);
                g.globalAlpha((k < len ? 1.0 : 0.3) * 0.35);
                g.fillRect(x + 2, y, colW - 4, bottom - y);
                g.globalAlpha(k < len ? 1.0 : 0.3);
                g.fillRect(x + 2, y - 1.5, colW - 4, 3);
                if (lane == 1 && v >= 0.999) {
                    // A tie: a bridge into the next step.
                    g.fillRect(x + colW - 2, y - 1.5, 4, 3);
                }
            }
            // The gate row: filled when on, fainter for a lower chance.
            const bool on = gates[k].on();
            g.globalAlpha((k < len ? 1.0 : 0.3) * (on ? 0.35 + 0.65 * chances[k].value() : 1.0));
            g.fillStyle(on ? (k == playingStep && shownPlaying.get() ? Theme::text : Theme::accent)
                           : Theme::recess);
            g.beginPath();
            g.roundRect(x + 2, kGateY, colW - 4, kGateH, 3);
            g.fill();
            if (!on) {
                g.strokeStyle(Theme::line);
                g.lineWidth(1.0);
                g.stroke();
            }
            g.globalAlpha(1.0);
        }
        // Beat marks under every fourth step.
        g.fillStyle(Theme::textFaint);
        for (int k = 0; k < kSteps; k += 4)
            g.fillRect(kGridX + k * colW + 2, kGateY + kGateH + 3, 2, 2);
    }

    void setLane(int k, double y) {
        const double u = clamp((kLaneTop + kLaneH - y) / kLaneH, 0.0, 1.0);
        if (lane == 0) editPitch(k, roundToInt(-24.0 + 48.0 * u));
        if (lane == 1) lengths[k].edit(0.05 + 0.95 * u);
        if (lane == 2) chances[k].edit(roundToInt(u * 20.0) / 20.0);
    }

    void pointer(const Pointer& p) {
        const int k = clamp(floorToInt(column(p.x())), 0, kSteps - 1);
        const bool inside = p.x() >= kGridX && p.x() < kGridX + kGridW;
        if (p.action() == PointerAction::Press) {
            dragMode = 0;
            if (p.y() >= kTabsY && p.y() < kTabsY + 15 && inside) {
                lane = clamp(floorToInt((p.x() - kGridX) / 80.0), 0, 2);
            } else if (p.over(kGridX, kLaneTop - 4, kGridW, kLaneH + 8)) {
                dragMode = 1;
                setLane(k, p.y());
            } else if (p.over(kGridX, kGateY - 2, kGridW, kGateH + 4)) {
                dragMode = 2;
                paintOn = !gates[k].on();
                gates[k].edit(paintOn ? 1.0 : 0.0);
                lastPainted = k;
            }
        } else if (p.action() == PointerAction::Drag) {
            if (dragMode == 1) setLane(k, p.y());
            if (dragMode == 2 && k != lastPainted) {
                gates[k].edit(paintOn ? 1.0 : 0.0);
                lastPainted = k;
            }
        } else if (p.action() == PointerAction::Release) {
            dragMode = 0;
            lastPainted = -1;
        } else if (p.action() == PointerAction::Wheel && p.over(kGridX, kLaneTop, kGridW, kLaneH)) {
            if (lane == 0) editPitch(k, roundToInt(pitchShown(k) + p.wheel()));
            if (lane == 1) lengths[k].edit(lengths[k].value() + 0.05 * p.wheel());
            if (lane == 2) chances[k].edit(chances[k].value() + 0.05 * p.wheel());
        }
    }
};
