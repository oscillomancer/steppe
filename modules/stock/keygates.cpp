// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Key Gates: notes to gates by key, for drums played from a keyboard or a
// clip. Each of its eight lanes has a key, a MIDI note number (60 is the
// pitch outputs' C4, so 36 is C2); its Out is high while any voice holds
// that key, and its Vel is the velocity of the note that last started
// there, held. A note that starts on a key another voice still holds drops
// the gate for a frame, so a drum after it hits again. The defaults are the
// General MIDI drum keys: kick, snare, clap, closed hat, open hat, low tom,
// rimshot, cowbell.

constexpr int kLanes = 8;

struct KeyGates : Module {
    static constexpr ModuleInfo info = {
        .name = "Key Gates",
        .id = "com.suppleaudio.rack.keygates",
        .vendor = "Supple Audio",
        .description =
            "Eight gates and velocities from notes, one per key: drums played from notes.",
        .tags = {"controller", "drum", "utility"}};

    Param key_1 = integer("Key 1", 0, 127, 36).help("Lane 1's key: 36 is C2, a kit's kick.");
    Param key_2 = integer("Key 2", 0, 127, 38).help("Lane 2's key: 38 is D2, a snare.");
    Param key_3 = integer("Key 3", 0, 127, 39).help("Lane 3's key: 39 is D#2, a clap.");
    Param key_4 = integer("Key 4", 0, 127, 42).help("Lane 4's key: 42 is F#2, a closed hat.");
    Param key_5 = integer("Key 5", 0, 127, 46).help("Lane 5's key: 46 is A#2, an open hat.");
    Param key_6 = integer("Key 6", 0, 127, 45).help("Lane 6's key: 45 is A2, a low tom.");
    Param key_7 = integer("Key 7", 0, 127, 37).help("Lane 7's key: 37 is C#2, a rimshot.");
    Param key_8 = integer("Key 8", 0, 127, 56).help("Lane 8's key: 56 is G#3, a cowbell.");

    Input pitch = pitchIn("Pitch").help("Each voice's key, 1 per octave, 0 at C4: Notes' pitch.");
    Input gate = gateIn("Gate").help("Each voice's gate: Notes' gate.");
    Input velocity =
        unipolarIn("Velocity")
            .normal(1.0)
            .help("Each voice's velocity, read as its gate rises; 1 while unconnected.");

    Outputs<kLanes> out = gateOut("Out").help("High while a voice holds the lane's key.");
    Outputs<kLanes> vel =
        unipolarOut("Vel").help("The velocity of the note that last started on the key.");

    struct Voice {
        bool on = false;
        int key = -1;  // the key it holds while on
    };
    Array<Voice, 16> voices;
    Array<bool, 16> rose;          // this frame: the voice's gate rose
    Array<double, kLanes> struck;  // each lane's velocity, held
    Array<bool, kLanes> gap;       // this frame: a note started on a key held the last
    Array<bool, kLanes> held;      // this frame: a voice holds the key
    Array<bool, kLanes> wasHeld;   // the last frame's held
    Array<int, kLanes> keys;       // this block's keys

    int keyOf(int lane) const {
        switch (lane) {
            case 0:
                return roundToInt(key_1.value());
            case 1:
                return roundToInt(key_2.value());
            case 2:
                return roundToInt(key_3.value());
            case 3:
                return roundToInt(key_4.value());
            case 4:
                return roundToInt(key_5.value());
            case 5:
                return roundToInt(key_6.value());
            case 6:
                return roundToInt(key_7.value());
            default:
                return roundToInt(key_8.value());
        }
    }

    void prepare() {
        for (int c = 0; c < 16; ++c) voices[c] = Voice{};
        for (int k = 0; k < kLanes; ++k) {
            struck[k] = 0.0;
            wasHeld[k] = false;
        }
    }

    void process(int frames) {
        for (int k = 0; k < kLanes; ++k) {
            keys[k] = keyOf(k);
            out[k].setChannels(1);
            vel[k].setChannels(1);
        }
        const int n = min(max(gate.channels(), 1), 16);
        // Voices the cable no longer carries hold nothing: a gate already
        // high when the channel comes back is a new note.
        for (int c = n; c < 16; ++c) voices[c] = Voice{};
        for (int i = 0; i < frames; ++i) {
            // Every voice's gate first, then the lanes, so the order of the
            // voices does not matter when one hands a key to another.
            for (int c = 0; c < n; ++c) {
                Voice& v = voices[c];
                const double g = gate[c][i];
                const bool was = v.on;
                v.on = v.on ? g >= 0.25 : g >= 0.5;
                rose[c] = v.on && !was;
                if (rose[c]) v.key = roundToInt(60.0 + 12.0 * pitch[c][i]);
            }
            for (int k = 0; k < kLanes; ++k) {
                held[k] = false;
                gap[k] = false;
                for (int c = 0; c < n; ++c) {
                    if (!voices[c].on || voices[c].key != keys[k]) continue;
                    held[k] = true;
                    if (!rose[c]) continue;
                    struck[k] = clamp(velocity[c][i], 0.0, 1.0);
                    // Held the frame before, by any voice: low for this
                    // frame, so the drum hits again.
                    if (wasHeld[k]) gap[k] = true;
                }
                out[k][0][i] = held[k] && !gap[k] ? 1.0 : 0.0;
                vel[k][0][i] = struck[k];
                wasHeld[k] = held[k];
            }
        }
    }

    // The inputs along the top; then two banks of four lanes, each lane a
    // column: its key, its gate, its velocity.
    void panel(Panel& p) {
        p.size(220, 400);
        p.input(pitch, 50, 52);
        p.input(gate, 110, 52);
        p.input(velocity, 170, 52);
        p.knob(key_1, 30, 112);
        p.knob(key_2, 83, 112);
        p.knob(key_3, 137, 112);
        p.knob(key_4, 190, 112);
        p.knob(key_5, 30, 256);
        p.knob(key_6, 83, 256);
        p.knob(key_7, 137, 256);
        p.knob(key_8, 190, 256);
        for (int k = 0; k < kLanes; ++k) {
            const double x = 30.0 + 53.3 * imod(k, 4);
            const double y = k < 4 ? 160.0 : 304.0;
            p.output(out[k], x, y);
            p.output(vel[k], x, y + 44.0);
        }
    }
};
