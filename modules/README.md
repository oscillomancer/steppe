# Modules

The modules of Steppe's modular racks (ADR-0062, `docs/specs/rack.md`). A
module is one C++ file in the DSP script language, a struct deriving from
`Module`: its ports, its knobs and its `process()`. Steppe compiles a module
the moment it is saved; stock modules are also compiled into the app.
Sharing a module is sharing its file. The language: `skill/guides/modules.md`.
The rules for this folder: `AGENTS.md`.

```cpp
#include <supple/script.h>

struct Fold : Module {
    static constexpr ModuleInfo info = {.name = "Fold", .id = "com.example.fold",
                                        .tags = {"waveshaper"}};
    Input in = audioIn("In").help("What it folds.");
    Param folds = percent("Folds", 30).help("How far past the edge the wave folds.");
    Output out = audioOut("Out").help("The folded wave.");

    void process(int frames) {
        for (int c = 0; c < channels(); ++c)
            for (int i = 0; i < frames; ++i)
                out[c][i] = sin(in[c][i] * (1 + 8 * folds.at(i, c)));
    }
};
```

## Stock modules

All `com.suppleaudio.rack.<name>`. Host modules (written in the engine:
Out, In, Notes, Transport, Plugin) are in `docs/specs/rack.md` §5.

### Sources, filters, shapers and effects

| Module | What it is |
|---|---|
| `vca` | An amplifier a CV opens, exponential or linear |
| `vco` | The analog-style workhorse: sine, triangle, saw, pulse and sub at once, exp and through-zero FM, hard sync, poly |
| `filter` | A multimode state-variable filter: low, band, high, notch and 24 dB low pass at once, pitch tracking, steady self-oscillation, drive |
| `noise` | White, pink, brown and blue noise, crackle and clocked digital noise (hiss, metallic buzz, steps), poly |
| `delay` | A stereo tape-style delay up to 10 s: free or synced, ping-pong, feedback past 100 %, tone, wow and flutter, a send and return to patch into the loop |
| `reverb` | A lush stereo reverb, an eight-line feedback delay network: size, decay to freeze, damping, diffusion, modulation, shimmer, pre-delay |
| `ladder` | A four-pole ladder low pass with saturating feedback and bass compensation; 12 dB and resonant high-pass taps, poly |
| `fold` | A west-coast wavefolder, 4x oversampled: fold, symmetry, stages; and a soft-clip output |
| `lpg` | A vactrol-style low-pass gate: a ping makes plucks and bongos of any source; CV mode, resonance, its envelope as an output, poly |
| `string` | A plucked string, a waveguide per channel: decay, damping, brightness, pluck position, stiffness, velocity and an exciter input |
| `resonator` | A 24-mode modal resonator: structure morphs string, beam, bar, bell and plate; brightness, decay, damping, position, stereo spread, poly |
| `drum` | An analog-style drum voice, eight models on a knob (kick, snare, tom, closed and open hat, clap, cowbell, rimshot): tune, tone, decay, snap, accent, poly |
| `morph` | A morphing oscillator: one Shape from sine through triangle, saw, square and narrow pulse to formant bursts, a phase Warp, a sub; anti-aliased, poly |
| `operator` | A phase-modulation operator for FM stacks: ratio, fine, Hz offset, PM index, self-feedback, level; outputs its sine and its phase, poly |
| `phasor` | A phase generator: a 0..1 ramp at the pitch with skew, offset, sync and a wrap trigger, poly |
| `shaper` | Phase in, wave out by a curve (sine, triangle, saw, pulse, smooth step, window) with skew and fold: phase distortion by cables, poly |
| `harmonic` | An additive oscillator, up to 32 partials: brightness, odd/even, stretch, a spectral comb that sweeps; odd and even outputs, never aliases, poly |
| `drive` | A distortion with seven curves (soft, tube, hard clip, fold, rectify, bitcrush, downsample): drive, bias, tone, mix, 4x oversampled, poly |
| `chorus` | A stereo chorus ensemble of up to six voices, and a flanger: rate, depth, spread, feedback either way, mix, poly |
| `shift` | A frequency shifter through zero, up and down at once, 50 dB+ sideband rejection: shift, fine, CV depth, spiralling feedback; a ring modulator with its own carrier or a patched one, poly |
| `granular` | A granular processor on a live four-second buffer, 32 grains: size, density, position, spray, pitch and scatter, reverse chance, stereo spread, freeze by toggle or gate |
| `vowel` | A formant filter of four bands morphing through a, e, i, o, u at audio rate: size (formant shift), resonance, mix, poly |
| `comb` | A tuned comb filter that rings at the pitch: feedback positive (every harmonic) or negative (odd only), damping in the loop kept in tune, mix, poly |
| `wavetable` | A wavetable oscillator with seven tables of its own (classic, sync, fold, formant, vowel, organ, digital): position to sweep, unison with detune, a display of the wave, never aliases, poly |
| `pitchshift` | A pitch shifter, two octaves either way: two delay taps cross-fading in step with the input (a held note keeps its pitch), fine, window size, feedback for shimmer, mix, poly |
| `freeze` | A spectral freeze: a gate or switch holds the spectrum and plays it on, partials at their exact pitches; blur to smear in time, shift by semitones keeping each partial whole, tilt, poly |
| `orbit` | A circular panner: the sound turns round four or eight speakers at a rate either way, angle, width from a point to everywhere, poly voices spread round the ring, Doppler in a room to 10 m across, a stereo fold-down, reset |
| `glitch` | A clocked glitch: at each step (clock or the song's grid) a chance of a stutter, a reverse, a pitch drop or a tape stop, by weights; slices in note values that Vary halves for rolls; seeded, with a reset; an active gate, poly |
| `compressor` | A compressor: threshold, ratio, soft knee, attack, release, makeup, mix; a key input to duck by with a high-pass on what it hears; linked or per channel; its gain as a CV; a curve and gain-reduction display, poly |

### Modulation, sequencing, generative and utilities

| Module | What it is |
|---|---|
| `adsr` | An envelope with curved stages, gated, triggered or looping, with an end trigger |
| `lfo` | Seven shapes, free or locked to the song position, morphable, with poly spread |
| `sequencer` | Sixteen steps of notes, gates, lengths (ties) and chances; five walks; clock or song sixteenths |
| `random` | Clocked random notes and three gates with a looping memory that mutates by Change; scales; poly |
| `function` | Rise and fall with a curve: an envelope, a cycling LFO, a slew limiter or a follower |
| `quantizer` | Pitch to the nearest note of 22 scales or a custom one, transposed in degrees; keyboard face |
| `clock` | Six ratios (/16 to x8) of the song, a free tempo or an incoming clock, with swing and a reset out |
| `euclid` | A Euclidean rhythm with its own accent spread, gate, accent and off-beat outputs, circle face |
| `turing` | Shift Register: a looping random register, locked to chaotic by Flip, with a CV and four bit gates |
| `chance` | A coin toss per gate to A or B, latched as a random switch, or a burst of ratchets; poly |
| `chord` | A note to a poly chord: 22 types or a key's own thirds, inversions, voicings and a strum |
| `arp` | An arpeggiator over held poly notes: six orders, octaves, gate length, latch |
| `slew` | A slew limiter and portamento, linear to exponential, that can leave leaps alone; poly |
| `hold` | Sample and hold or track and hold, noise when nothing is in, a value per voice; poly |
| `follower` | An envelope follower, peak or RMS, with a gate above a threshold and an onset trigger; poly |
| `chaos` | A Lorenz or Thomas attractor: X, Y and Z as slow organic CVs, poly copies that drift apart |
| `rhythm` | Drum patterns from a map of nine grooves morphed by X and Y, densities per voice, chaos, accents |
| `mixer` | Four channels with level, pan and mute to stereo and mono, through a master; poly summed |
| `scale` | Four attenuverters with offsets; a row with nothing in it is a constant |
| `math` | Sum, difference, product, min, max, abs, a crossfade and a comparator of A and B; poly |
| `logic` | AND, OR, XOR, NOT, a flip-flop and a trigger on every change; poly |
| `switcher` | A sequential switch both ways (four to one, one to four), stepped or selected, with a crossfade; poly |
| `macro` | Eight knobs out of eight jacks, unipolar or bipolar, glided: a patch's performance controls |
| `poly` | Merge eight monos to a poly cable, split one to eight, sum it, or spread it across stereo |
| `scope` | An oscilloscope: two traces with afterglow, triggered, free or locked to the song, and X-Y |
| `pitch` | A constant pitch or a transposer: octave, semitone and fine on a poly pitch, with a tuner display |
| `drift` | Slow, smooth random motion across poly voices, apart or following one shared path |
| `ratchet` | Gates into bursts of quick pulses, by chance or every Nth gate, curved and fading; poly |
| `markov` | A melody by the odds of which note follows which: four styles, or learned from a melody played in |
| `cellular` | A 1-D cellular automaton: up to sixteen cells grown by a rule, as poly gates, four taps and a CV |
| `looper` | Records a CV and a gate over a beat to eight bars in time with the song and loops them; punch in by the loop |
| `stages` | A function generator of two to six stages (level, time, curve): once, looping, or gated with a hold; poly |
| `matrix` | Four inputs to four outputs through sixteen attenuverters; an unpatched input is an offset; poly |
| `polyrhythm` | Four rhythms of 1 to 16 beats against a beat to four bars of the song: ramps, gates, Any and Hits |
| `life` | An 8x8 Game of Life (five rules) played a column a clock: eight poly gates, a CV, counts and a generation trigger |
| `lsystem` | L-System: a self-similar melody grown by rewriting A, B and C, each moving the tune by scale degrees |
