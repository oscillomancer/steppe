// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Bracken: echoes, from a clean slapback to a smeared, shifting, reversed wash.
//
// A bracken frond repeats itself down its stem, each leaflet a smaller copy
// of the one before; so do echoes. Each channel's loop is two halves in a
// ring, so every second echo can come early or late (groove) while each pair
// keeps the beat. Each half:
//
//   Reads    with two heads that crossfade: they glide to a new time,
//            bending the pitch as tape changing speed does, or fade to it;
//            the same heads shift each echo's pitch from the one before.
//   Passes   the echo through the loop: diffusion (allpasses that blur each
//            pass a little more), the tone (low and high cut), the medium
//            (clean, tape or a bucket brigade, worn by age) and drive into
//            the medium's curve.
//   Feeds    the other half at the feedback's level; past 100% a soft limit
//            holds the loop.
//
// The echoes leave forward or each reversed, ducked under the input, then
// width and mix. Freeze holds the loop exactly: the halves read whole
// samples and hand them on untouched, so a held loop never dulls.

constexpr double kLongest = 5.0;       // seconds: the longest time between echoes
constexpr double kShortest = 0.001;    // and the shortest
constexpr double kSliceLongest = 2.5;  // seconds: the longest reversed slice
constexpr double kWindow = 0.06;       // seconds: the heads' crossfade window
constexpr double kModReach = 0.004;    // seconds either way at full modulation
constexpr double kGlide = 0.07;        // seconds: each of a bend's two lags
constexpr double kDiffuseTotal = 0.0202;
constexpr double kDuckFloor = -60.0;  // dBFS: below this the input ducks nothing
constexpr double kDuckSpan = 30.0;    // and this much above it ducks fully
constexpr int kTrail = 128;           // the ducking strip's points: 4 s

// The diffusers' lengths, seconds: 20.2 ms together.
double diffuseSeconds(int k) {
    if (k == 0) return 0.00231;
    if (k == 1) return 0.00373;
    if (k == 2) return 0.00587;
    return 0.00829;
}

// Quarter notes between echoes for each synced time (0: free).
double noteBeats(int k) {
    switch (k) {
        case 1:
            return 0.125;
        case 2:
            return 1.0 / 6.0;
        case 3:
            return 0.25;
        case 4:
            return 0.375;
        case 5:
            return 1.0 / 3.0;
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

// --- the media ---------------------------------------------------------------------------
//
// Age wears each its own way. Clean is an early digital unit: fewer bits and
// a narrower band. Tape loses highs (and wobbles, in the modulation). A
// bucket brigade's clock slows as the time grows, so a longer echo is darker
// and its hold coarser; age gives it fewer stages.

double cleanHz(double age) { return 20000.0 * (1.0 - 0.6 * age * age); }
double cleanBits(double age) { return 24.0 - 13.0 * age * age; }
double tapeHz(double age) { return 19000.0 * (1.0 - 0.7 * pow(clamp(age, 0.0, 1.0), 0.8)); }
double bbdClock(double seconds, double age) {
    return 10000.0 * (1.0 - 0.7 * age) / (2.0 * max(seconds, kShortest));
}
double bbdHz(double clock, double rate) { return min(0.4 * clock, min(18000.0, 0.45 * rate)); }

// The medium's curve, slope 1 at rest: clean clips with a firm knee, tape
// rounds over, the bucket brigade leans (even harmonics).
double mediumCurve(int m, double x) {
    if (m == 0) return x / pow(1.0 + x * x * x * x, 0.25);
    if (m == 1) return tanh(x);
    const double b = 0.3, t = tanh(b);
    return (tanh(x + b) - t) / (1.0 - t * t);
}

// The loop's guard, on what each half hands on (never the first echo):
// untouched up to -4.4 dBFS, then easing toward full scale, where a loop fed
// back past 100% settles.
double guard(double x) {
    const double a = abs(x);
    if (a <= 0.6) return x;
    return sign(x) * (0.6 + 0.4 * tanh((a - 0.6) / 0.4));
}

// The echoes' guard as they leave: two halves each held near full scale
// would sum past it. Untouched up to -1.9 dBFS, never past full scale.
double ceiling(double x) {
    const double a = abs(x);
    if (a <= 0.8) return x;
    return sign(x) * (0.8 + 0.2 * tanh((a - 0.8) / 0.2));
}

// A smooth random value per cycle, the same for a cycle wherever it plays.
double hashUnit(int n) {
    double x = wrap(n, 65521) + 17.0;
    for (int k = 0; k < 4; ++k) x = fmod(x * x + 12345.0 + k, 65521.0);
    return x / 32760.0 - 1.0;
}

// The random endpoints stay fixed for a whole cycle. Only the interpolation
// moves each sample; both sides keep the same hashes as a direct lookup.
struct CycleNoise {
    int cycle = -1;
    Array<double, 2> from, to;

    double value(int side, int n, int stride, double phase) {
        if (n != cycle) {
            cycle = n;
            for (int c = 0; c < 2; ++c) {
                from[c] = hashUnit(n * stride + c);
                to[c] = hashUnit(n * stride + c + stride);
            }
        }
        const double a = from[side], b = to[side];
        return a + (b - a) * (0.5 - 0.5 * cos(pi * phase));
    }
};

// --- the loop's parts --------------------------------------------------------------------

// A line of the past. read(d) is d samples before the sample about to be
// pushed, between samples by a cubic (Hermite); d stays 2 or more.
struct Ring {
    Buffer<float> data;
    int size = 4, at = 0;

    void prepare(int n) {
        data.resize(n);
        size = n;
        clear();
    }
    void clear() {
        data.fill(0.0f);
        at = 0;
    }
    void push(double x) {
        data[at] = static_cast<float>(x);
        at = at + 1;
        if (at >= size) at = 0;
    }
    double tap(int d) const {
        int i = at - d;
        if (i < 0) i += size;
        return data[i];
    }
    double read(double d) const {
        const double c = clamp(d, 2.0, size - 3.0);
        const int k = floorToInt(c);
        const double f = c - k;
        const double ym = tap(k - 1), y0 = tap(k), y1 = tap(k + 1), y2 = tap(k + 2);
        const double c1 = 0.5 * (y1 - ym);
        const double c2 = ym - 2.5 * y0 + 2.0 * y1 - 0.5 * y2;
        const double c3 = 0.5 * (y2 - ym) + 1.5 * (y0 - y1);
        return ((c3 * f + c2) * f + c1) * f + y0;
    }
};

// Two heads half a window apart, each fading in and out over its window
// (sin^2, so the two always sum to one). A head's time is held for its window
// when time changes fade, or follows the glide when they bend; a pitch ratio
// other than 1 sweeps it through the window (negative: backwards).
struct Heads {
    double phase = 0.0;  // head 0's place in its window; head 1's is half on
    double held0 = 0.0, held1 = 0.0;
    double d0 = 0.0, d1 = 0.0, w0 = 1.0;
    bool one = true;  // both heads at one place: a single read

    void start(double time) {
        phase = 0.0;
        held0 = time;
        held1 = time;
        d0 = time;
        d1 = time;
        w0 = 0.0;
        one = true;
    }
    void advance(double time, double ratio, double window, double mod, bool hold) {
        const double was = phase;
        phase += 1.0 / window;
        if (phase >= 1.0) {
            phase -= floor(phase);
            held0 = time;
        }
        if (was < 0.5 && phase >= 0.5) held1 = time;
        if (!hold) {
            held0 = time;
            held1 = time;
        }
        double p1 = phase + 0.5;
        if (p1 >= 1.0) p1 -= 1.0;
        const double sweep = window * (ratio - 1.0);
        d0 = held0 + (0.5 - phase) * sweep + mod;
        d1 = held1 + (0.5 - p1) * sweep + mod;
        one = d0 == d1;
        if (!one) {
            const double s = sin(pi * phase);
            w0 = s * s;
        }
    }
    double sample(const Ring& r, double less) const {
        if (one) return r.read(d0 - less);
        return w0 * r.read(d0 - less) + (1.0 - w0) * r.read(d1 - less);
    }
};

// Reversed slices: a head that runs backwards through the past for a slice,
// then the next, which fades in over the last few milliseconds of the one
// before. Each slice's time is cut as it starts, centred on the echo's time.
struct Slices {
    // Per head: samples run, its length, its first delay, and its fades'
    // lengths (in as the slice before faded out; out as asked when it began).
    Array<double, 2> elapsed, length, from, fadeIn, fadeOut;
    Array<bool, 2> on;
    int now = 0;  // the head of the newest slice

    void clear() {
        on[0] = false;
        on[1] = false;
        now = 0;
    }
    // reach: how far the slice reaches either side of the time (samples);
    // speed: the pitch ratio's size (the head runs back at it).
    void advance(double time, double reach, double speed, double fadeSamples) {
        for (int h = 0; h < 2; ++h)
            if (on[h]) {
                elapsed[h] += 1.0;
                if (elapsed[h] >= length[h]) on[h] = false;
            }
        if (!on[now] || elapsed[now] >= length[now] - fadeOut[now]) {
            const int next = on[now] ? 1 - now : now;
            fadeIn[next] = on[now] ? fadeOut[now] : fadeSamples;
            fadeOut[next] = fadeSamples;
            length[next] = max(2.0 * reach / (1.0 + speed), fadeIn[next] + fadeOut[next] + 2.0);
            from[next] = time - 0.5 * (1.0 + speed) * length[next];
            elapsed[next] = 0.0;
            on[next] = true;
            now = next;
        }
    }
    double weight(int h) const {
        const double e = elapsed[h], fi = fadeIn[h], fo = fadeOut[h];
        double w = 1.0;
        if (e < fi)
            w = sin(0.5 * pi * e / fi);
        else if (e > length[h] - fo)
            w = sin(0.5 * pi * max(0.0, length[h] - e) / fo);
        return w * w;
    }
    double sample(const Ring& r, double speed, double mod, double less) const {
        double y = 0.0;
        for (int h = 0; h < 2; ++h)
            if (on[h]) y += weight(h) * r.read(from[h] + (1.0 + speed) * elapsed[h] + mod - less);
        return y;
    }
};

// An allpass, w = x + g w[L], y = w[L] - g w: a pure delay of L when g is 0.
struct Allpass {
    Buffer<float> line;
    int size = 1, at = 0, length = 1;

    void prepare(int n) {
        line.resize(n + 1);
        size = n + 1;
        length = n;
        clear();
    }
    void clear() {
        line.fill(0.0f);
        at = 0;
    }
    double process(double x, double g) {
        int i = at - length;
        if (i < 0) i += size;
        const double old = line[i];
        const double w = x + g * old;
        line[at] = static_cast<float>(w);
        at = at + 1;
        if (at >= size) at = 0;
        return old - g * w;
    }
};

struct Diffuser {
    Array<Allpass, 4> stage;
    bool running = false;
    void clear() {
        for (Allpass& a : stage) a.clear();
        running = false;
    }
    double process(double x, double g) {
        running = true;
        double y = x;
        for (int k = 0; k < 4; ++k) y = stage[k].process(y, k == 1 || k == 3 ? -g : g);
        return y;
    }
};

// What every pass shares this sample: the medium's weights, the tone's, the
// drive's.
struct Tone {
    double clean = 1.0, tape = 0.0, analog = 0.0;  // the media, crossfading
    double lowOn = 0.0, highOn = 0.0;              // the cuts, in or out
    double cleanBand = 0.0, step = 0.0;            // clean's filter's share; its quantum (0: none)
    double tapeCoef = 1.0;                         // tape's one-pole
    double driveGain = 1.0, driveBlend = 0.0;
    double atAge = -1.0, atDrive = -1.0;

    // Reuse exact values only: a smoothed control still follows every sample.
    void configure(double age, double drive, double rate) {
        if (age != atAge) {
            atAge = age;
            cleanBand = clamp(age * 4.0, 0.0, 1.0);
            const double bits = cleanBits(age);
            step = bits < 23.5 ? 1.0 / exp2(bits - 1.0) : 0.0;
            tapeCoef = 1.0 - exp(-twoPi * tapeHz(age) / rate);
        }
        if (drive != atDrive) {
            atDrive = drive;
            driveGain = dbToGain(drive);
            driveBlend = clamp(drive, 0.0, 1.0);
        }
    }
};

// One pass through the loop's colour. The media that are silent are cleared,
// so each starts afresh when it comes back.
struct Pass {
    Biquad lowCut, highCut;
    Biquad cleanLow;
    double tapeZ = 0.0;
    Biquad bbdIn, bbdOut;
    double holdPhase = 1.0, held = 0.0;
    OnePole dc;
    bool cleanRunning = false, tapeRunning = false, analogRunning = false;
    double bbdAt = -1.0;  // the clock its filters are designed for

    void clear() {
        lowCut.clear();
        highCut.clear();
        cleanLow.clear();
        tapeZ = 0.0;
        bbdIn.clear();
        bbdOut.clear();
        holdPhase = 1.0;
        held = 0.0;
        dc.clear();
        cleanRunning = false;
        tapeRunning = false;
        analogRunning = false;
    }

    double drive(int m, double x, const Tone& t) const {
        if (t.driveBlend <= 0.0) return x;
        const double y = mediumCurve(m, x * t.driveGain) / t.driveGain;
        return x + t.driveBlend * (y - x);
    }

    double process(double x, const Tone& t, double clock, double rate) {
        double y = x;
        const double lc = lowCut.process(y);
        y = y + t.lowOn * (lc - y);
        const double hc = highCut.process(y);
        y = y + t.highOn * (hc - y);
        double sum = 0.0;
        if (t.clean > 0.0) {
            const double f = cleanLow.process(y);
            double c = y + t.cleanBand * (f - y);
            if (t.step > 0.0) c = round(c / t.step) * t.step;
            sum += t.clean * drive(0, c, t);
            cleanRunning = true;
        } else if (cleanRunning) {
            cleanLow.clear();
            cleanRunning = false;
        }
        if (t.tape > 0.0) {
            tapeZ += t.tapeCoef * (y - tapeZ);
            sum += t.tape * drive(1, tapeZ, t);
            tapeRunning = true;
        } else if (tapeRunning) {
            tapeZ = 0.0;
            tapeRunning = false;
        }
        if (t.analog > 0.0) {
            const double v = bbdIn.process(y);
            // The bucket brigade holds each sample for a clock period; at or
            // above the host's rate there is nothing to hold.
            const double step = clock / rate;
            if (step >= 1.0) {
                held = v;
                holdPhase = 1.0;
            } else {
                holdPhase += step;
                if (holdPhase >= 1.0) {
                    holdPhase -= floor(holdPhase);
                    held = v;
                }
            }
            const double a = dc.highpass(drive(2, bbdOut.process(held), t));
            sum += t.analog * a;
            analogRunning = true;
        } else if (analogRunning) {
            bbdIn.clear();
            bbdOut.clear();
            holdPhase = 1.0;
            held = 0.0;
            dc.clear();
            analogRunning = false;
        }
        return sum;
    }
};

// One half of a channel's loop: its line, its heads (forward, for the loop,
// and reversed, for the output), its diffusers and passes for each.
struct Half {
    Ring ring;
    Heads head;   // the loop's
    Slices back;  // the reversed output's
    Diffuser diffuse, diffuseBack;
    Pass pass, passBack;
    double lag = 0.0, time = 0.0;  // samples: a bend's two lags; time follows the second
    double target = 0.0;
    double chain = 0.0;  // the diffusers' path, 0 (direct) to 1
    int frozen = 2;      // whole samples, while frozen
    double out = 0.0;    // this sample's echo
    double feed = 0.0;   // what goes on to the other half
    bool backRunning = false;

    void clear() {
        ring.clear();
        diffuse.clear();
        diffuseBack.clear();
        pass.clear();
        passBack.clear();
        head.start(time);
        back.clear();
        out = 0.0;
        feed = 0.0;
        backRunning = false;
    }
};

// --- the look ----------------------------------------------------------------------------

constexpr int kInk = 0x1A1411;
constexpr int kGroundTop = 0x2B221B;
constexpr int kGroundBottom = 0x201914;
constexpr int kGraphTop = 0x1D1713;
constexpr int kGraphBottom = 0x130F0C;
constexpr int kGrid = 0x2B231D;
constexpr int kGridLit = 0x3E332A;
constexpr int kLinen = 0xE3D5BE;
constexpr int kKnobLight = 0x54463B;
constexpr int kKnobDark = 0x2A221C;
constexpr int kMeterLow = 0x6E7A4B;
constexpr int kMeterHigh = 0xD8CE9C;
constexpr int kFrond = 0xA3BC6E;   // a young frond: clean, and the right side's echoes
constexpr int kStraw = 0xD8B46A;   // tape
constexpr int kCopper = 0xC27C4A;  // the frond in autumn: analog, and a darkened echo
constexpr int kFrost = 0xB7CFD6;   // held
constexpr int kStem = 0x7E6A4E;

// A colour between two, t of the way from a to b.
int blend(int a, int b, double t) {
    const double u = clamp(t, 0.0, 1.0);
    const int ar = idiv(a, 65536), ag = imod(idiv(a, 256), 256), ab = imod(a, 256);
    const int br = idiv(b, 65536), bg = imod(idiv(b, 256), 256), bb = imod(b, 256);
    const int r = roundToInt(ar + (br - ar) * u), g = roundToInt(ag + (bg - ag) * u);
    const int bl = roundToInt(ab + (bb - ab) * u);
    return r * 65536 + g * 256 + bl;
}

struct Bracken : Effect {
    static constexpr Info info = {
        .name = "Bracken",
        .id = "com.suppleaudio.bracken",
        .vendor = "Supple Audio",
        .version = "1.0.0",
        .description =
            "Echoes in time or free: groove, ping-pong, tone and diffusion in the loop, pitch "
            "shift and reverse, clean, tape and analog media, ducking, freeze, and readouts of "
            "the echo time and decay.",
        .features = {"delay"},
        .tail = -1};

    // --- time ---------------------------------------------------------------------------------

    Param sync = choice("Sync",
                        {"Free", "1/32", "1/16 T", "1/16", "1/16 D", "1/8 T", "1/8", "1/8 D",
                         "1/4 T", "1/4", "1/4 D", "1/2 T", "1/2", "1/2 D", "1/1"},
                        9)
                     .keys({"free", "n32", "n16t", "n16", "n16d", "n8t", "n8", "n8d", "n4t", "n4",
                            "n4d", "n2t", "n2", "n2d", "n1"})
                     .in("Time")
                     .help("The time between echoes in note values at the song's tempo, or free.");
    Param time = milliseconds("Time", 1, 5000, 400)
                     .in("Time")
                     .activeWhen(sync, {"Free"})
                     .help("The time between echoes when sync is free.");
    Param groove =
        percent("Groove", 0, -50, 50)
            .smoothed(30)
            .in("Time")
            .help(
                "Every second echo later (positive) or earlier, each pair still on the beat: "
                "+33% swings straight echoes to a triplet feel.");
    Param offset =
        percent("Offset", 0, -50, 50)
            .smoothed(30)
            .in("Time")
            .help(
                "The right side's echoes later (positive) or earlier, as a share of the time: "
                "width from a centred source; in ping-pong, the bounce back from the right.");
    Param timeChanges =
        choice("Time Changes", {"Bend", "Fade"}, 0)
            .in("Time")
            .help(
                "How a new time is reached: bend glides there, bending the pitch as tape "
                "changing speed does; fade crossfades to it with no bend.");

    // --- the loop -----------------------------------------------------------------------------

    Param feedback =
        percent("Feedback", 40, 0, 110)
            .smoothed(20)
            .in("Loop")
            .help(
                "Each echo's level against the one before; past 100% they grow until the loop's "
                "soft limit holds them.");
    Param routing = choice("Routing", {"Stereo", "Ping-Pong", "Mono"}, 0)
                        .in("Loop")
                        .help(
                            "Stereo keeps each side's echoes to itself; ping-pong bounces the "
                            "sum left, right, left; mono echoes the sum in the centre.");
    Param lowCut =
        frequency("Low Cut", 20, 2000, 80)
            .smoothed(30)
            .in("Loop")
            .help("A high-pass in the loop: each echo thinner than the last (20 Hz: off).");
    Param highCut =
        frequency("High Cut", 1000, 20000, 9000)
            .smoothed(30)
            .in("Loop")
            .help("A low-pass in the loop: each echo darker than the last (20 kHz: off).");
    Param diffusion =
        percent("Diffusion", 0)
            .smoothed(30)
            .in("Loop")
            .help(
                "Blurs each echo a little more than the last, from distinct repeats to a wash "
                "(allpasses in the loop; times under 24 ms stay distinct).");
    Param shift =
        semitones("Shift", -12, 12, 0)
            .smoothed(30)
            .in("Loop")
            .help(
                "The pitch each echo moves from the one before: 12 climbs an octave a repeat, "
                "small values drift and thicken.");
    Param reverse = toggle("Reverse", false)
                        .in("Loop")
                        .help("Plays each echo backwards, in slices as long as the time.");

    // --- character ----------------------------------------------------------------------------

    Param medium =
        choice("Medium", {"Clean", "Tape", "Analog"}, 0)
            .in("Character")
            .help(
                "What the echoes pass through: clean digital; tape, which softens the highs and "
                "wobbles; or an analog bucket brigade, darker the longer the time.");
    Param age = percent("Age", 30)
                    .smoothed(50)
                    .in("Character")
                    .help(
                        "The medium's wear: clean loses bits and band like an early digital unit, "
                        "tape dulls and wobbles more, analog darkens and coarsens.");
    Param drive =
        decibels("Drive", 0, 24, 0)
            .smoothed(20)
            .in("Character")
            .help(
                "Saturation in the loop, each echo pushed again through the medium's curve: "
                "quiet echoes pass at their level, loud ones thicken and flatten from this far "
                "below full scale. 0 dB is clean.");
    Param modulation =
        percent("Modulation", 0)
            .smoothed(30)
            .in("Character")
            .help("How far the echo time wanders, up to 4 ms either way: chorus to seasick.");
    Param modRate = frequency("Mod Rate", 0.05, 10, 0.8)
                        .smoothed(30)
                        .in("Character")
                        .activeAbove(modulation, 0)
                        .help("How fast it wanders.");

    // --- output -------------------------------------------------------------------------------

    Param duck = decibels("Duck", 0, 24, 0)
                     .smoothed(20)
                     .in("Output")
                     .help(
                         "Turns the echoes down by up to this while the input plays (fully above "
                         "-30 dBFS), back up in the gaps.");
    Param freeze =
        toggle("Freeze", false)
            .in("Output")
            .help("Holds what is in the loop, repeating it forever; the input is shut out.");
    Param width = percent("Width", 100, 0, 200)
                      .smoothed(20)
                      .in("Output")
                      .help("The echoes' stereo width: 0 is mono, 200 twice as wide.");
    Param input = decibels("Input", -48, 12, 0)
                      .offAtMin()
                      .smoothed(20)
                      .in("Output")
                      .help(
                          "How much of the input enters the echoes (the dry sound is untouched): "
                          "automate it to throw only some words in.");
    Param mix = percent("Mix", 25).smoothed(20).in("Output").help(
        "Dry and echoes: the dry stays full up to 50% and the echoes reach full "
        "there; 100% is echoes alone.");
    Param output = decibels("Output", -24, 24, 0).smoothed(20).in("Output").help("The level out.");

    Param inputLevel = decibels("Input Level", -96, 24, -96)
                           .meter()
                           .help("The input's peak, dBFS, falling 20 dB a second.");
    Param outputLevel = decibels("Output Level", -96, 24, -96)
                            .meter()
                            .help("The output's peak, dBFS, falling 20 dB a second.");
    Param echoLevel = decibels("Echo Level", -96, 24, -96)
                          .meter()
                          .help("The echoes' peak before the mix, dBFS, falling 20 dB a second.");
    Param echoTime = milliseconds("Echo Time", 0, 10000, 0)
                         .meter()
                         .help("The time between echoes now, at the song's tempo when synced.");
    Param decay =
        milliseconds("Decay", 0, 600000, 0)
            .meter()
            .help(
                "How long the echoes take to fall 60 dB below the first at 1 kHz (feedback, "
                "tone and medium; drive aside). Held or self-oscillating loops read 600 s.");
    Param duckNow =
        decibels("Duck Now", -24, 0, 0).meter().help("How far the echoes are ducked this moment.");

    void factoryPatches(Patches& p) {
        p.add("Quarter Echo");
        p.add("Vocal Slap")
            .set(sync, "free")
            .set(time, 95)
            .set(feedback, 8)
            .set(lowCut, 150)
            .set(highCut, 6500)
            .set(medium, "tape")
            .set(age, 20)
            .set(mix, 18);
        p.add("Dotted Throw")
            .set(sync, "n8d")
            .set(feedback, 45)
            .set(lowCut, 180)
            .set(highCut, 5500)
            .set(duck, 10)
            .set(mix, 30);
        p.add("Swung Ping-Pong")
            .set(sync, "n8")
            .set(routing, "ping_pong")
            .set(groove, 33)
            .set(feedback, 50)
            .set(highCut, 7000);
        p.add("Wide Double")
            .set(sync, "free")
            .set(time, 14)
            .set(offset, 50)
            .set(feedback, 0)
            .set(lowCut, 120)
            .set(highCut, 12000)
            .set(modulation, 25)
            .set(modRate, 0.6)
            .set(mix, 30);
        p.add("Tape Echo")
            .set(sync, "free")
            .set(time, 320)
            .set(feedback, 55)
            .set(medium, "tape")
            .set(age, 55)
            .set(drive, 6)
            .set(modulation, 15)
            .set(modRate, 0.7)
            .set(highCut, 6000);
        p.add("Bucket Brigade")
            .set(sync, "n4")
            .set(feedback, 60)
            .set(medium, "analog")
            .set(age, 40)
            .set(drive, 4)
            .set(modulation, 30)
            .set(modRate, 0.9);
        p.add("Dub Spiral")
            .set(sync, "n4d")
            .set(feedback, 98)
            .set(medium, "tape")
            .set(age, 45)
            .set(drive, 9)
            .set(lowCut, 220)
            .set(highCut, 3500)
            .set(modulation, 10)
            .set(mix, 35);
        p.add("Shimmer")
            .set(sync, "n4")
            .set(feedback, 65)
            .set(shift, 12)
            .set(diffusion, 70)
            .set(lowCut, 300)
            .set(highCut, 10000)
            .set(timeChanges, "fade")
            .set(mix, 30);
        p.add("Falling Steps")
            .set(sync, "n8")
            .set(feedback, 60)
            .set(shift, -5)
            .set(routing, "ping_pong")
            .set(mix, 30);
        p.add("Reverse Swell")
            .set(sync, "n2")
            .set(feedback, 35)
            .set(reverse, true)
            .set(diffusion, 30)
            .set(mix, 40);
        p.add("Ambient Smear")
            .set(sync, "free")
            .set(time, 450)
            .set(feedback, 75)
            .set(diffusion, 85)
            .set(medium, "tape")
            .set(age, 35)
            .set(modulation, 30)
            .set(modRate, 0.4)
            .set(mix, 35);
        p.add("Early Digital").set(sync, "n8").set(feedback, 45).set(age, 90).set(highCut, 8000);
        p.add("Centre Echo")
            .set(sync, "n4")
            .set(routing, "mono")
            .set(feedback, 30)
            .set(highCut, 7000)
            .set(duck, 6);
    }

    // --- audio --------------------------------------------------------------------------------

    double rate = 48000.0;
    Array<Half, 4> half;  // left A, left B, right A, right B
    Array<double, 4> diffuseLength;
    Tone tone;
    double diffuseSum = 0.0;  // samples: the diffusers' delay together

    // Glides of their own, all snapped until the first block.
    double cleanW = 1.0, tapeW = 0.0, analogW = 0.0, mediumStep = 0.001;
    double lowW = 0.0, highW = 0.0, cutStep = 0.001;
    double frozenW = 0.0, freezeStep = 0.001;
    double backW = 0.0, backStep = 0.001;
    double routeGain = 1.0, routeStep = 0.001;
    int routeNow = 0;
    double chainStep = 0.001;
    double glideCoef = 0.001;
    bool unplayed = true;

    // Modulation: a phase per side, and tape's own wow and flutter.
    double modPhase = 0.0, wowPhase = 0.0, flutterPhase = 0.0;
    int modCycle = 0, wowCycle = 0;
    CycleNoise modNoise, wowNoise;

    // The filters' designs: what they were last designed for.
    double atLow = -1.0, atHigh = -1.0, atClean = -1.0;

    // Ducking and the onsets the editor's playhead starts from.
    double duckEnv = 0.0, duckUp = 0.1, duckDown = 0.001, duckGain = 1.0;
    double duckDb = 0.0, duckAttack = 0.01, duckRelease = 0.001;
    double fastEnv = 0.0, slowEnv = 0.0, fastUp = 0.1, fastDown = 0.01, slowUp = 0.01,
           slowDown = 0.001;
    int sinceHit = 1000000000, hitHold = 4800;

    double tempoNow = 120.0, baseSeconds = 0.5;
    double tailAt = -1.0, decaySeconds = 0.0;
    double peakIn0 = 0.0, peakIn1 = 0.0, peakOut0 = 0.0, peakOut1 = 0.0, peakEcho = 0.0,
           fall = 0.999;
    double trailIn = 0.0, trailEcho = 0.0, trailDuck = 0.0;
    int trailAt = 0, trailCount = 0, trailLength = 1500;

    Shared<double> inLeft, inRight, outLeft, outRight, echoShown, sinceHitShown;
    Shared<double> tempoShown, rateShown, decayShown, modPhaseShown;
    Shared<double> timeA0, timeB0, timeA1, timeB1;  // seconds: each half's time now
    Shared<int> routeShown, trailHead;
    Shared<bool> frozenShown;
    SharedArray<float, 128> trailInShown, trailEchoShown, trailDuckShown;

    void prepare() {
        rate = sampleRate();
        tone.atAge = -1.0;
        tone.atDrive = -1.0;
        const int length = ceilToInt((kLongest + kSliceLongest + 2.0 * kModReach + 0.05) * rate);
        diffuseSum = 0.0;
        for (int k = 0; k < 4; ++k) {
            diffuseLength[k] = roundToInt(diffuseSeconds(k) * rate);
            diffuseSum += diffuseLength[k];
        }
        for (int h = 0; h < 4; ++h) {
            Half& s = half[h];
            s.ring.prepare(length);
            for (int k = 0; k < 4; ++k) {
                s.diffuse.stage[k].prepare(toInt(diffuseLength[k]));
                s.diffuseBack.stage[k].prepare(toInt(diffuseLength[k]));
            }
            s.pass.dc.setCutoff(2.0);
            s.passBack.dc.setCutoff(2.0);
            s.time = 0.1 * rate;
            s.lag = s.time;
            s.target = s.time;
            s.chain = 0.0;
            s.clear();
        }
        mediumStep = 1.0 / (0.03 * rate);
        cutStep = 1.0 / (0.01 * rate);
        freezeStep = 1.0 / (0.02 * rate);
        backStep = 1.0 / (0.03 * rate);
        routeStep = 1.0 / (0.008 * rate);
        chainStep = 1.0 / (0.02 * rate);
        glideCoef = 1.0 - exp(-1.0 / (kGlide * rate));
        duckUp = 1.0 - exp(-1.0 / (0.001 * rate));
        duckDown = 1.0 - exp(-1.0 / (0.06 * rate));
        duckAttack = 1.0 - exp(-1.0 / (0.005 * rate));
        duckRelease = 1.0 - exp(-1.0 / (0.15 * rate));
        fastUp = 1.0 - exp(-1.0 / (0.0005 * rate));
        fastDown = 1.0 - exp(-1.0 / (0.03 * rate));
        slowUp = 1.0 - exp(-1.0 / (0.03 * rate));
        slowDown = 1.0 - exp(-1.0 / (0.3 * rate));
        fall = dbToGain(-20.0 / rate);
        hitHold = roundToInt(0.12 * rate);
        trailLength = max(1, roundToInt(4.0 * rate / kTrail));
        // A restart prepares an instance that has played: start it afresh.
        routeNow = routing.index();
        routeGain = 1.0;
        frozenW = 0.0;
        backW = 0.0;
        modPhase = 0.0;
        wowPhase = 0.0;
        flutterPhase = 0.0;
        modCycle = 0;
        wowCycle = 0;
        modNoise.cycle = -1;
        wowNoise.cycle = -1;
        duckEnv = 0.0;
        duckDb = 0.0;
        duckGain = 1.0;
        fastEnv = 0.0;
        slowEnv = 0.0;
        sinceHit = 1000000000;
        tempoNow = 120.0;
        tailAt = -1.0;
        peakIn0 = 0.0;
        peakIn1 = 0.0;
        peakOut0 = 0.0;
        peakOut1 = 0.0;
        peakEcho = 0.0;
        trailIn = 0.0;
        trailEcho = 0.0;
        trailDuck = 0.0;
        trailAt = 0;
        trailCount = 0;
        atLow = -1.0;
        atHigh = -1.0;
        atClean = -1.0;
        for (int h = 0; h < 4; ++h) {
            half[h].pass.bbdAt = -1.0;
            half[h].passBack.bbdAt = -1.0;
        }
        unplayed = true;
        snapWeights();
        retune(lowCut.value(), highCut.value(), age.value());
    }

    void snapWeights() {
        const int m = medium.index();
        cleanW = m == 0 ? 1.0 : 0.0;
        tapeW = m == 1 ? 1.0 : 0.0;
        analogW = m == 2 ? 1.0 : 0.0;
        lowW = lowCut > 20.5 ? 1.0 : 0.0;
        highW = highCut < 19500.0 ? 1.0 : 0.0;
        frozenW = freeze.on() ? 1.0 : 0.0;
        backW = reverse.on() ? 1.0 : 0.0;
        routeNow = routing.index();
        routeGain = 1.0;
    }

    void update() {
        if (unplayed) snapWeights();
        // The tempo is the last one heard (120 until then).
        const int k = sync.index();
        baseSeconds = k == 0 ? time * 0.001 : noteBeats(k) * 60.0 / tempoNow;
        reportTail();
    }

    // The cuts and clean's band, for these values: only what moved.
    void retune(double lc, double hc, double ag) {
        if (abs(lc - atLow) > 1e-3 * lc) {
            atLow = lc;
            for (int h = 0; h < 4; ++h) {
                half[h].pass.lowCut.highpass(lc, 0.7071067811865476);
                half[h].passBack.lowCut.highpass(lc, 0.7071067811865476);
            }
        }
        if (abs(hc - atHigh) > 1e-3 * hc) {
            atHigh = hc;
            const double f = min(hc, 0.45 * rate);
            for (int h = 0; h < 4; ++h) {
                half[h].pass.highCut.lowpass(f, 0.7071067811865476);
                half[h].passBack.highCut.lowpass(f, 0.7071067811865476);
            }
        }
        const double ch = cleanHz(ag);
        if (abs(ch - atClean) > 1e-3 * ch) {
            atClean = ch;
            const double f = min(ch, 0.45 * rate);
            for (int h = 0; h < 4; ++h) {
                half[h].pass.cleanLow.lowpass(f, 0.7071067811865476);
                half[h].passBack.cleanLow.lowpass(f, 0.7071067811865476);
            }
        }
    }

    // A bucket brigade's filters follow its half's clock.
    void retuneBbd(Pass& p, double clock) {
        if (abs(clock - p.bbdAt) <= 0.01 * clock) return;
        p.bbdAt = clock;
        const double f = bbdHz(clock, rate);
        p.bbdIn.lowpass(f, 0.7071067811865476);
        p.bbdOut.lowpass(f, 0.7071067811865476);
    }

    // --- what the loop does to an echo, predicted -----------------------------------------

    // One pass's gain at hz, for a half this many seconds long: the cuts and
    // the medium as set (Butterworth magnitudes; tape's one-pole exactly).
    double passGain(double hz, double seconds, double r) const {
        double g = 1.0;
        if (lowCut > 20.5) {
            const double u = hz / lowCut;
            g *= u * u / sqrt(1.0 + u * u * u * u);
        }
        if (highCut < 19500.0) {
            const double u = hz / min(highCut.value(), 0.45 * r);
            g *= 1.0 / sqrt(1.0 + u * u * u * u);
        }
        const int m = medium.index();
        if (m == 0) {
            const double u = hz / min(cleanHz(age), 0.45 * r);
            const double lp = 1.0 / sqrt(1.0 + u * u * u * u);
            g *= 1.0 + clamp(age * 4.0, 0.0, 1.0) * (lp - 1.0);
        } else if (m == 1) {
            const double a = 1.0 - exp(-twoPi * tapeHz(age) / r);
            const double w = twoPi * hz / r;
            const double re = 1.0 - (1.0 - a) * cos(w), im = (1.0 - a) * sin(w);
            g *= a / sqrt(re * re + im * im);
        } else {
            const double u = hz / bbdHz(bbdClock(seconds, age), r);
            g *= 1.0 / (1.0 + u * u * u * u);
        }
        return g;
    }

    // Each loop alternates A and B: use their mean time and geometric mean
    // loss per echo. Analog's clock (and thus loss) can differ in each half.
    double fallSeconds(double hz, double a, double b, double db, double r) const {
        const double g = feedback * sqrt(passGain(hz, a, r) * passGain(hz, b, r));
        if (g >= 0.99999 || freeze.on()) return -1.0;
        if (g <= 1e-9) return 0.0;
        return db / (-20.0 * log10(g)) * (0.5 * (a + b));
    }

    // The same clamp order as the audio targets: first groove, then offset.
    // Mono has one centred loop; ping-pong offsets only its second half.
    double loopTime(int h, int route) const {
        const double g = groove.value(), right = 1.0 + offset;
        const double t = baseSeconds * (imod(h, 2) == 0 ? 1.0 + g : 1.0 - g);
        const double samples = clamp(t, kShortest, kLongest) * rate;
        const bool shifted = (route == 0 && h >= 2) || (route == 1 && h == 1);
        return (shifted ? clamp(samples * right, kShortest * rate, kLongest * rate) : samples) /
               rate;
    }

    // The tail the host is told: the echoes to -80 dB at the slowest of a
    // few frequencies, the last slice reversed, and the diffusers' ring.
    void reportTail() {
        tailAt = baseSeconds;
        const int route = channels() < 2 ? 2 : routing.index();
        const int loops = route == 0 ? 2 : 1;
        decaySeconds = 0.0;
        double longest = 0.0;
        double longestHalf = 0.0;
        bool forever = false;
        for (int c = 0; c < loops; ++c) {
            const double a = loopTime(2 * c, route), b = loopTime(2 * c + 1, route);
            longestHalf = max(longestHalf, max(a, b));
            const double d = fallSeconds(1000.0, a, b, 60.0, rate);
            if (d < 0.0 || decaySeconds < 0.0)
                decaySeconds = -1.0;
            else
                decaySeconds = max(decaySeconds, d);
            for (int k = 0; k < 4; ++k) {
                const double hz = k == 0 ? 60.0 : (k == 1 ? 250.0 : (k == 2 ? 1000.0 : 4000.0));
                const double t = fallSeconds(hz, a, b, 80.0, rate);
                if (t < 0.0) forever = true;
                longest = max(longest, t);
            }
        }
        if (forever) {
            setTail(-1);
            return;
        }
        const double extra = 1.5 * longestHalf +
                             (reverse.on() ? min(1.5 * longestHalf, kSliceLongest) : 0.0) +
                             0.5 * diffusion + 0.1;
        setTail(min(roundToInt((longest + extra) * rate), 2000000000));
    }

    // --- the sound ----------------------------------------------------------------------------

    // The time between echoes, seconds, at this transport.
    double baseTime(const Block& io) const {
        const int s = sync.index();
        if (s == 0) return time * 0.001;
        const double tempo = io.transport.tempo > 0.0 ? io.transport.tempo : 120.0;
        return noteBeats(s) * 60.0 / tempo;
    }

    // The modulation at this sample for a side, in samples, before its cap.
    double wobble(int side, double depth, double wear, int m) {
        double ph = modPhase + (side == 1 ? 0.25 : 0.0);
        if (ph >= 1.0) ph -= 1.0;
        double v = 0.0;
        if (m == 2) {
            v = ph < 0.25 ? 4.0 * ph : (ph < 0.75 ? 2.0 - 4.0 * ph : 4.0 * ph - 4.0);
        } else if (m == 1) {
            // Tape: a sine with a drifting random part, and age's own wow and
            // flutter on top.
            const double drift = modNoise.value(side, modCycle, 2, modPhase);
            v = 0.7 * sin(twoPi * ph) + 0.3 * drift;
        } else {
            v = sin(twoPi * ph);
        }
        double s = depth * kModReach * v;
        if (m == 1) {
            const double wow = wowNoise.value(side, wowCycle, 3, wowPhase);
            s += wear * (0.00035 * wow + 0.00006 * sin(twoPi * (flutterPhase + 0.3 * side)));
        }
        return s * rate;
    }

    void process(Block& io) {
        const int chans = io.channels;
        tempoNow = io.transport.tempo > 0.0 ? io.transport.tempo : 120.0;
        baseSeconds = baseTime(io);
        // update() handles every control change; between updates only the
        // transport can change the time used by these analytic estimates.
        if (baseSeconds != tailAt) reportTail();
        const bool bend = timeChanges.index() == 0;
        const bool frozenNow = freeze.on();
        const int routeWant = chans < 2 ? 2 : routing.index();
        const int m = medium.index();
        for (int i = 0; i < io.frames; ++i) {
            // The times each half is heading for.
            const double g = groove.at(i), o = offset.at(i);
            const double t = baseSeconds;
            const double ta = clamp(t * (1.0 + g), kShortest, kLongest) * rate;
            const double tb = clamp(t * (1.0 - g), kShortest, kLongest) * rate;
            // (While frozen the halves read the whole samples they held; these
            // are where they go on from.)
            const double right = 1.0 + o;
            half[0].target = ta;
            half[1].target =
                routeNow == 1 ? clamp(tb * right, kShortest * rate, kLongest * rate) : tb;
            half[2].target = clamp(ta * right, kShortest * rate, kLongest * rate);
            half[3].target = clamp(tb * right, kShortest * rate, kLongest * rate);
            for (int h = 0; h < 4; ++h) {
                Half& s = half[h];
                if (unplayed || !bend) {
                    s.lag = s.target;
                    s.time = s.target;
                } else {
                    s.lag += glideCoef * (s.target - s.lag);
                    s.time += glideCoef * (s.lag - s.time);
                }
                if (unplayed) {
                    s.head.start(s.time);
                    s.back.clear();
                    s.frozen = max(2, roundToInt(s.time));
                    s.chain = s.time >= diffuseSum + 0.003 * rate ? 1.0 : 0.0;
                }
            }
            unplayed = false;

            // The glides of the switches.
            cleanW = m == 0 ? min(1.0, cleanW + mediumStep) : max(0.0, cleanW - mediumStep);
            tapeW = m == 1 ? min(1.0, tapeW + mediumStep) : max(0.0, tapeW - mediumStep);
            analogW = m == 2 ? min(1.0, analogW + mediumStep) : max(0.0, analogW - mediumStep);
            const double lc = lowCut.at(i), hc = highCut.at(i), ag = age.at(i);
            lowW = lc > 20.5 ? min(1.0, lowW + cutStep) : max(0.0, lowW - cutStep);
            highW = hc < 19500.0 ? min(1.0, highW + cutStep) : max(0.0, highW - cutStep);
            retune(lc, hc, ag);
            const double was = frozenW;
            frozenW = frozenNow ? min(1.0, frozenW + freezeStep) : max(0.0, frozenW - freezeStep);
            if (was == 0.0 && frozenW > 0.0)
                for (int h = 0; h < 4; ++h) half[h].frozen = max(2, roundToInt(half[h].time));
            backW = reverse.on() ? min(1.0, backW + backStep) : max(0.0, backW - backStep);
            if (routeWant != routeNow) {
                routeGain -= routeStep;
                if (routeGain <= 0.0) {
                    routeGain = 0.0;
                    // The right loop is silent in ping-pong and mono: it starts
                    // empty when stereo comes back.
                    if (routeWant != 0) {
                        half[2].clear();
                        half[3].clear();
                    }
                    routeNow = routeWant;
                }
            } else {
                routeGain = min(1.0, routeGain + routeStep);
            }

            tone.clean = cleanW;
            tone.tape = tapeW;
            tone.analog = analogW;
            tone.lowOn = lowW;
            tone.highOn = highW;
            tone.configure(ag, drive.at(i), rate);

            // Modulation.
            const double depth = modulation.at(i) * (1.0 - frozenW);
            const double wear = m == 1 ? ag * (1.0 - frozenW) : 0.0;
            modPhase += modRate.at(i) / rate;
            if (modPhase >= 1.0) {
                modPhase -= 1.0;
                modCycle += 1;
            }
            wowPhase += 0.55 / rate;
            if (wowPhase >= 1.0) {
                wowPhase -= 1.0;
                wowCycle += 1;
            }
            flutterPhase += 7.1 / rate;
            if (flutterPhase >= 1.0) flutterPhase -= 1.0;
            const double mod0 = wobble(0, depth, wear, m), mod1 = wobble(1, depth, wear, m);

            const double sh = shift.at(i);
            const double ratio = sh == 0.0 ? 1.0 : exp2(sh / 12.0);
            const double fb = feedback.at(i);
            const double dg = 0.72 * diffusion.at(i);
            const double in = input.at(i);
            const double inGain = in <= -47.99 ? 0.0 : dbToGain(in) * (1.0 - frozenW);

            // The input.
            double l = io[0][i];
            double r = chans > 1 ? io[1][i] : l;
            if (!isFinite(l)) l = 0.0;
            if (!isFinite(r)) r = 0.0;
            const double mono = 0.5 * (l + r);

            // Ducking: the input's peak, fully ducking 30 dB above the floor;
            // the ducking itself lets go over 150 ms.
            const double level = max(abs(l), abs(r));
            duckEnv += (level > duckEnv ? duckUp : duckDown) * (level - duckEnv);
            const double over = clamp((gainToDb(duckEnv) - kDuckFloor) / kDuckSpan, 0.0, 1.0);
            const double want = duck.at(i) * over;
            duckDb += (want > duckDb ? duckAttack : duckRelease) * (want - duckDb);
            const double ducked = duckDb;
            duckGain = dbToGain(-ducked);

            // Onsets, for the editor's playhead.
            const double heard = level * (in <= -47.99 ? 0.0 : dbToGain(in));
            fastEnv += (heard > fastEnv ? fastUp : fastDown) * (heard - fastEnv);
            slowEnv += (heard > slowEnv ? slowUp : slowDown) * (heard - slowEnv);
            sinceHit = min(sinceHit + 1, 1000000000);
            if (fastEnv > 2.0 * slowEnv && fastEnv > 0.006 && sinceHit > hitHold) sinceHit = 0;

            // The loops.
            const int loops = routeNow == 0 ? 2 : 1;
            for (int c = 0; c < loops; ++c) {
                const double mod = c == 0 ? mod0 : mod1;
                stepHalf(half[2 * c], ratio, mod, bend, fb, dg, ag);
                stepHalf(half[2 * c + 1], ratio, mod, bend, fb, dg, ag);
                const double into = routeNow == 0 ? (c == 0 ? l : r) : mono;
                half[2 * c].ring.push(into * inGain + half[2 * c + 1].feed);
                half[2 * c + 1].ring.push(half[2 * c].feed);
            }

            // The echoes.
            double wl = 0.0, wr = 0.0;
            if (routeNow == 0) {
                wl = half[0].out + half[1].out;
                wr = half[2].out + half[3].out;
            } else if (routeNow == 1) {
                wl = half[0].out;
                wr = half[1].out;
            } else {
                wl = half[0].out + half[1].out;
                wr = wl;
            }
            wl = ceiling(wl);
            wr = ceiling(wr);
            const double wd = width.at(i);
            if (wd != 1.0) {
                const double mid = 0.5 * (wl + wr), side = 0.5 * (wl - wr) * wd;
                wl = mid + side;
                wr = mid - side;
            }
            const double wetGain = duckGain * routeGain;
            wl *= wetGain;
            wr *= wetGain;
            const double mx = mix.at(i);
            const double dry = min(1.0, 2.0 * (1.0 - mx)), wet = min(1.0, 2.0 * mx);
            const double gainOut = dbToGain(output.at(i));
            const double ol = (l * dry + wl * wet) * gainOut;
            const double orr = (r * dry + wr * wet) * gainOut;
            io[0][i] = ol;
            if (chans > 1) io[1][i] = orr;

            // Meters.
            peakIn0 = max(abs(l), peakIn0 * fall);
            peakIn1 = max(abs(r), peakIn1 * fall);
            peakOut0 = max(abs(ol), peakOut0 * fall);
            peakOut1 = max(abs(orr), peakOut1 * fall);
            const double echo = max(abs(wl), abs(wr));
            peakEcho = max(echo, peakEcho * fall);
            trailIn = max(trailIn, level);
            trailEcho = max(trailEcho, echo);
            trailDuck = max(trailDuck, ducked);
            trailCount += 1;
            if (trailCount >= trailLength) {
                trailInShown[trailAt] = static_cast<float>(gainToDb(trailIn));
                trailEchoShown[trailAt] = static_cast<float>(gainToDb(trailEcho));
                trailDuckShown[trailAt] = static_cast<float>(trailDuck);
                trailAt = trailAt + 1;
                if (trailAt >= kTrail) trailAt = 0;
                trailIn = 0.0;
                trailEcho = 0.0;
                trailDuck = 0.0;
                trailCount = 0;
            }
        }
        publish(io);
    }

    // One half for one sample: read, pass, and what it hands on.
    void stepHalf(Half& s, double ratio, double mod, bool bend, double fb, double dg, double ag) {
        const double cap = 0.45 * s.time;
        const double md = clamp(mod, -cap, cap);
        // The heads' window: shorter for a shift on a short time, so they
        // never reach past the newest sample.
        double window = kWindow * rate;
        const double reach = abs(ratio - 1.0);
        if (reach > 1e-9) window = min(window, 1.9 * (s.time - 2.0 - abs(md)) / reach);
        window = max(window, 32.0);
        s.head.advance(s.time, ratio, window, md, !bend);
        // The diffusers' path, once the time is long enough to hold them.
        const bool chainWanted = s.time >= diffuseSum + 0.003 * rate;
        s.chain = chainWanted ? min(1.0, s.chain + chainStep) : max(0.0, s.chain - chainStep);
        double x = 0.0;
        if (frozenW < 1.0) {
            if (s.chain < 1.0) x = s.head.sample(s.ring, 0.0);
            if (s.chain > 0.0) {
                const double d = s.diffuse.process(s.head.sample(s.ring, diffuseSum), dg);
                x = x + s.chain * (d - x);
            } else if (s.diffuse.running) {
                s.diffuse.clear();
            }
        }
        const double held = s.ring.tap(s.frozen);
        x = x + frozenW * (held - x);
        const double clock = bbdClock(s.time / rate, ag);
        if (analogW > 0.0) retuneBbd(s.pass, clock);
        const double y = s.pass.process(x, tone, clock, rate);
        const double sent = guard(fb * y);
        s.feed = sent + frozenW * (held - sent);
        // Reversed: its own heads, slices as long as the time (2.5 s at most).
        double out = y;
        if (backW > 0.0) {
            const double reach = max(2.0, min(s.time - 2.0 - abs(md), kSliceLongest * rate));
            s.back.advance(s.time, reach, ratio, min(0.01 * rate, 0.25 * reach));
            double z = 0.0;
            if (s.chain < 1.0) z = s.back.sample(s.ring, ratio, md, 0.0);
            if (s.chain > 0.0) {
                const double d =
                    s.diffuseBack.process(s.back.sample(s.ring, ratio, md, diffuseSum), dg);
                z = z + s.chain * (d - z);
            } else if (s.diffuseBack.running) {
                s.diffuseBack.clear();
            }
            if (analogW > 0.0) retuneBbd(s.passBack, clock);
            const double yb = s.passBack.process(z, tone, clock, rate);
            out = y + backW * (yb - y);
            s.backRunning = true;
        } else if (s.backRunning) {
            s.diffuseBack.clear();
            s.passBack.clear();
            s.back.clear();
            s.backRunning = false;
        }
        s.out = out;
    }

    void publish(const Block& io) {
        inputLevel.set(gainToDb(peakIn0 > peakIn1 ? peakIn0 : peakIn1));
        outputLevel.set(gainToDb(peakOut0 > peakOut1 ? peakOut0 : peakOut1));
        echoLevel.set(gainToDb(peakEcho));
        echoTime.set(baseSeconds * 1000.0);
        const double d = decaySeconds;
        decay.set(d < 0.0 ? 600000.0 : min(d * 1000.0, 600000.0));
        duckNow.set(gainToDb(duckGain));
        inLeft.set(gainToDb(peakIn0));
        inRight.set(gainToDb(peakIn1));
        outLeft.set(gainToDb(peakOut0));
        outRight.set(gainToDb(peakOut1));
        echoShown.set(gainToDb(peakEcho));
        sinceHitShown.set(sinceHit / rate);
        tempoShown.set(io.transport.tempo > 0.0 ? io.transport.tempo : 120.0);
        rateShown.set(rate);
        decayShown.set(d);
        // The place in the two cycles the editor draws, so its dot crosses both.
        modPhaseShown.set(((modCycle & 1) + modPhase) * 0.5);
        // Held, the loop repeats at the times it was frozen at.
        const bool held = frozenW > 0.5;
        timeA0.set((held ? half[0].frozen : half[0].time) / rate);
        timeB0.set((held ? half[1].frozen : half[1].time) / rate);
        timeA1.set((held ? half[2].frozen : half[2].time) / rate);
        timeB1.set((held ? half[3].frozen : half[3].time) / rate);
        routeShown.set(routeNow);
        trailHead.set(trailAt);
        frozenShown.set(frozenW > 0.5);
    }

    // --- the editor ---------------------------------------------------------------------------

    static constexpr double kW = 1080;
    static constexpr double kH = 780;
    static constexpr double mx = 32;  // the echo map
    static constexpr double my = 102;
    static constexpr double mw = 716;
    static constexpr double mh = 200;
    static constexpr double py = 368;  // the lower panels
    static constexpr double ph = 396;
    static constexpr double lx = 16;  // the loop panel
    static constexpr double lw = 420;
    static constexpr double cx = 448;  // the medium panel
    static constexpr double cw = 304;
    static constexpr double ox = 764;  // the output panel
    static constexpr double ow = 300;
    static constexpr double tgx = 32;  // the echo tone graph
    static constexpr double tgy = 540;
    static constexpr double tgw = 388;
    static constexpr double tgh = 148;
    static constexpr double gridX = 792;  // the note grid
    static constexpr double gridY = 154;
    static constexpr double cellW = 60;
    static constexpr double cellH = 22;
    static constexpr double trx = 780;  // the ducking strip
    static constexpr double tryy = 410;
    static constexpr double trw = 268;
    static constexpr double trh = 74;
    static constexpr double wvx = 464;  // the modulation's wave
    static constexpr double wvy = 646;
    static constexpr double wvw = 150;
    static constexpr double wvh = 50;

    // The editor's own.
    int dragging = 0;  // 1 a knob, 2 the echo map
    int dragKnob = -1;
    double knobPos = 0.0;
    int hotKnob = -1;
    double hoverX = -1.0, hoverY = -1.0;
    double pressX = 0.0, pressY = 0.0, pressTime = 400.0, pressFeedback = 0.4;
    int pressSync = 9;
    double span = 0.0;  // seconds across the map, easing to what the echoes need

    // The echoes the map draws: when (seconds), how loud (dB), which side
    // (0 left, 1 right, 2 both) and how far they have darkened (0..1).
    Array<double, 48> echoAt, echoDb, echoDark;
    Array<int, 48> echoSide, echoPasses;
    int echoes = 0;

    void editor(Editor& e) { e.size(kW, kH); }

    double rateNow() const { return rateShown.get() > 1000.0 ? rateShown.get() : 48000.0; }
    double bpm() const { return tempoShown.get() > 0.0 ? tempoShown.get() : 120.0; }
    int mediumColour(int m) const { return m == 0 ? kFrond : (m == 1 ? kStraw : kCopper); }

    // --- drawing helpers ----------------------------------------------------------------------

    void panel(Canvas& g, double x, double y, double w, double h) {
        g.linearGradient(0, y, 0, y + h, Theme::surface, kGroundBottom);
        g.beginPath();
        g.roundRect(x, y, w, h, 10);
        g.fill();
        g.strokeStyle(Theme::line);
        g.lineWidth(1);
        g.stroke();
        g.strokeStyle(Theme::lineLit);
        g.globalAlpha(0.5);
        g.beginPath();
        g.moveTo(x + 10, y + 0.5);
        g.lineTo(x + w - 10, y + 0.5);
        g.stroke();
        g.globalAlpha(1.0);
    }

    void well(Canvas& g, double x, double y, double w, double h) {
        g.linearGradient(0, y, 0, y + h, kGraphTop, kGraphBottom);
        g.beginPath();
        g.roundRect(x, y, w, h, 6);
        g.fill();
        g.strokeStyle(Theme::line);
        g.lineWidth(1);
        g.stroke();
    }

    void heading(Canvas& g, const char* text, double x, double y) {
        g.fillStyle(Theme::text);
        g.serifFont(14);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(text, x, y);
    }

    void aside(Canvas& g, const char* text, double x, double y, TextAlign align) {
        g.fillStyle(Theme::textFaint);
        g.serifItalicFont(10.5);
        g.textAlign(align);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(text, x, y);
    }

    void label(Canvas& g, const char* text, double x, double y, TextAlign align) {
        g.fillStyle(Theme::textFaint);
        g.boldFont(8.5);
        g.textAlign(align);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(text, x, y);
    }

    void pill(Canvas& g, double x, double y, double w, double h, const char* text, bool lit,
              int colour, bool hot) {
        g.fillStyle(lit ? colour : (hot ? 0x3A2F27 : Theme::raised));
        g.beginPath();
        g.roundRect(x, y, w, h, h * 0.5);
        g.fill();
        if (!lit) {
            g.strokeStyle(Theme::line);
            g.lineWidth(1);
            g.stroke();
        }
        g.fillStyle(lit ? kInk : Theme::textMuted);
        g.boldFont(9.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(text, x + w * 0.5, y + h * 0.5 + 0.5);
    }

    bool hovered(double x, double y, double w, double h) const {
        return dragging == 0 && hoverX >= x && hoverX <= x + w && hoverY >= y && hoverY <= y + h;
    }

    // A knob: an arc from 7 to 5 o'clock, lit to the value; a bipolar one
    // lights from the top.
    void dial(Canvas& g, double x, double y, double r, double norm, int colour, bool bipolar,
              bool hot, const char* name) {
        const double start = 0.75 * pi, sweep = 1.5 * pi;
        g.lineCap(LineCap::Round);
        g.strokeStyle(kInk);
        g.lineWidth(r > 34 ? 6 : 5);
        g.beginPath();
        g.arc(x, y, r, start, start + sweep);
        g.stroke();
        const double at = start + sweep * clamp(norm, 0.0, 1.0);
        const double from = bipolar ? start + 0.5 * sweep : start;
        g.strokeStyle(colour);
        g.lineWidth(r > 34 ? 4.5 : 3.5);
        if (abs(at - from) > 0.001) {
            g.beginPath();
            if (at >= from)
                g.arc(x, y, r, from, at);
            else
                g.arc(x, y, r, at, from);
            g.stroke();
        }
        g.radialGradient(x, y - r * 0.25, 2, r - 4, hot ? 0x5E4E41 : kKnobLight, kKnobDark);
        g.beginPath();
        g.circle(x, y, r - 7);
        g.fill();
        g.strokeStyle(Theme::text);
        g.lineWidth(2);
        g.beginPath();
        g.moveTo(x + cos(at) * (r - 16), y + sin(at) * (r - 16));
        g.lineTo(x + cos(at) * (r - 9), y + sin(at) * (r - 9));
        g.stroke();
        g.fillStyle(Theme::textFaint);
        g.boldFont(8.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(name, x, y - r - 12);
    }

    void valueStyle(Canvas& g) {
        g.fillStyle(Theme::text);
        g.monoFont(10.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
    }

    void fillHz(Canvas& g, double hz, double x, double y) {
        if (hz < 100.0)
            g.fillText(format("%.1f Hz", hz), x, y);
        else if (hz < 1000.0)
            g.fillText(format("%.0f Hz", hz), x, y);
        else
            g.fillText(format("%.1f kHz", hz * 0.001), x, y);
    }

    // A signed level; one that rounds to nothing reads 0.0.
    void fillDb(Canvas& g, double db, double x, double y) {
        if (abs(db) < 0.05)
            g.fillText("0.0 dB", x, y);
        else
            g.fillText(format("%+.1f dB", db), x, y);
    }

    void fillSeconds(Canvas& g, double s, double x, double y) {
        if (s < 1.0)
            g.fillText(format("%.0f ms", s * 1000.0), x, y);
        else if (s < 10.0)
            g.fillText(format("%.2f s", s), x, y);
        else
            g.fillText(format("%.1f s", s), x, y);
    }

    void fillNote(Canvas& g, int k, double x, double y) {
        switch (k) {
            case 0:
                g.fillText("FREE", x, y);
                break;
            case 1:
                g.fillText("1/32", x, y);
                break;
            case 2:
                g.fillText("1/16 T", x, y);
                break;
            case 3:
                g.fillText("1/16", x, y);
                break;
            case 4:
                g.fillText("1/16 D", x, y);
                break;
            case 5:
                g.fillText("1/8 T", x, y);
                break;
            case 6:
                g.fillText("1/8", x, y);
                break;
            case 7:
                g.fillText("1/8 D", x, y);
                break;
            case 8:
                g.fillText("1/4 T", x, y);
                break;
            case 9:
                g.fillText("1/4", x, y);
                break;
            case 10:
                g.fillText("1/4 D", x, y);
                break;
            case 11:
                g.fillText("1/2 T", x, y);
                break;
            case 12:
                g.fillText("1/2", x, y);
                break;
            case 13:
                g.fillText("1/2 D", x, y);
                break;
            default:
                g.fillText("1/1", x, y);
        }
    }

    // The note value nearest a time at the tempo, within 2%; 0 when none is.
    int nearestNote(double seconds) const {
        const double beats = seconds * bpm() / 60.0;
        int best = 0;
        double off = 0.02;
        for (int k = 1; k <= 14; ++k) {
            const double d = abs(log(noteBeats(k) / beats));
            if (d < off) {
                off = d;
                best = k;
            }
        }
        return best;
    }

    // The note values in order of length, for stepping through them.
    int noteByLength(int n) const {
        switch (n) {
            case 0:
                return 1;
            case 1:
                return 2;
            case 2:
                return 3;
            case 3:
                return 5;
            case 4:
                return 4;
            case 5:
                return 6;
            case 6:
                return 8;
            case 7:
                return 7;
            case 8:
                return 9;
            case 9:
                return 11;
            case 10:
                return 10;
            case 11:
                return 12;
            case 12:
                return 13;
            default:
                return 14;
        }
    }
    int lengthRank(int k) const {
        for (int n = 0; n < 14; ++n)
            if (noteByLength(n) == k) return n;
        return 8;
    }

    // --- the mark -----------------------------------------------------------------------------

    // A bracken frond: a stem arching over, its leaflets in pairs, each
    // smaller than the one before, as the echoes are. A leaflet brightens as
    // the playhead passes its echo; the frond browns with age.
    void drawMark(Canvas& g, double x, double y) {
        const int leafColour = blend(kFrond, kCopper, age.value());
        g.lineCap(LineCap::Round);
        g.strokeStyle(kStem);
        g.lineWidth(1.6);
        g.beginPath();
        g.moveTo(x - 6, y + 22);
        g.bezierCurveTo(x - 6, y + 6, x + 0, y - 14, x + 12, y - 18);
        g.stroke();
        const double since = sinceHitShown.get();
        for (int k = 0; k < 7; ++k) {
            const double t = 0.12 + k * 0.13;
            const double u = 1.0 - t;
            // Along the stem's curve.
            const double px = u * u * u * (x - 6) + 3.0 * u * u * t * (x - 6) +
                              3.0 * u * t * t * x + t * t * t * (x + 12);
            const double pyy = u * u * u * (y + 22) + 3.0 * u * u * t * (y + 6) +
                               3.0 * u * t * t * (y - 14) + t * t * t * (y - 18);
            const double size = 7.5 * pow(0.8, k);
            bool lit = false;
            if (k < echoes) lit = since >= echoAt[k] && since < echoAt[k] + 0.18;
            g.fillStyle(lit ? kLinen : leafColour);
            g.globalAlpha(lit ? 1.0 : 0.55 + 0.4 * pow(0.85, k));
            for (int s = -1; s <= 1; s += 2) {
                g.save();
                g.translate(px, pyy);
                g.rotate(s * 1.05 - 0.35 - 0.06 * k);
                g.beginPath();
                g.ellipse(size * 0.55 * s, 0, size * 0.62, size * 0.24);
                g.fill();
                g.restore();
            }
        }
        g.globalAlpha(1.0);
    }

    void drawLevel(Canvas& g, double x, double y, double w, double left, double right,
                   const char* name) {
        label(g, name, x - 8, y + 4, TextAlign::Right);
        for (int c = 0; c < 2; ++c) {
            const double db = c == 0 ? left : right;
            const double y0 = y + c * 6;
            g.fillStyle(kInk);
            g.beginPath();
            g.roundRect(x, y0, w, 4, 2);
            g.fill();
            const double t = clamp((db + 60.0) / 60.0, 0.0, 1.0);
            if (t > 0.0) {
                g.linearGradient(x, 0, x + w, 0, kMeterLow, db > -0.1 ? Theme::alert : kMeterHigh);
                g.beginPath();
                g.roundRect(x, y0, w * t, 4, 2);
                g.fill();
            }
        }
    }

    void drawHeader(Canvas& g) {
        drawMark(g, 28, 28);
        g.fillStyle(Theme::text);
        g.serifFont(23);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Alphabetic);
        g.fillText("Bracken", 54, 37);
        const double nameWidth = g.measureText("Bracken");
        g.fillStyle(Theme::textMuted);
        g.serifItalicFont(13);
        const int m = medium.index();
        const char* what = frozenShown.get() ? "held echoes"
                           : m == 0          ? "clean echoes"
                           : m == 1          ? "tape echoes"
                                             : "bucket-brigade echoes";
        g.fillText(what, 62 + nameWidth, 37);
        g.textBaseline(TextBaseline::Middle);
        drawLevel(g, 560, 23, 120, inLeft.get(), inRight.get(), "IN");
        drawLevel(g, 748, 23, 120, echoShown.get(), echoShown.get(), "ECHOES");
        drawLevel(g, 936, 23, 120, outLeft.get(), outRight.get(), "OUT");
    }

    // --- the echo map -------------------------------------------------------------------------

    // The echoes after a sound, as the loop is set: each half's time as it
    // is now, each echo's level and how far the loop has darkened it.
    void layoutEchoes() {
        const int route = routeShown.get();
        const double a0 = max(timeA0.get(), 0.001), b0 = max(timeB0.get(), 0.001);
        const double a1 = max(timeA1.get(), 0.001), b1 = max(timeB1.get(), 0.001);
        const double seconds = 0.5 * (a0 + b0);
        const double r = rateNow();
        const double mid = passGain(1000.0, seconds, r), top = passGain(5000.0, seconds, r);
        const double fb = frozenShown.get() ? 1.0 : feedback.value();
        const double stepDb = gainToDb(max(fb * mid, 1e-6));
        const double darkStep = max(0.0, gainToDb(mid) - gainToDb(max(top, 1e-6)));
        echoes = 0;
        const int sides = route == 0 ? 2 : 1;
        for (int c = 0; c < sides; ++c) {
            double t = 0.0;
            for (int k = 0; k < 24 && echoes < 48; ++k) {
                const double half = (imod(k, 2) == 0) ? (c == 0 ? a0 : a1) : (c == 0 ? b0 : b1);
                t += half;
                const double db = frozenShown.get() ? 0.0 : (k * stepDb + gainToDb(mid));
                if (db < -54.0 || t > 60.0) break;
                echoAt[echoes] = t;
                echoDb[echoes] = db;
                echoDark[echoes] = clamp((k + 1) * darkStep / 30.0, 0.0, 1.0);
                echoSide[echoes] = route == 0 ? c : (route == 1 ? imod(k, 2) : 2);
                echoPasses[echoes] = k + 1;
                echoes += 1;
            }
        }
        // Earliest first, for the mark's leaflets.
        for (int i = 1; i < echoes; ++i)
            for (int j = i; j > 0 && echoAt[j] < echoAt[j - 1]; --j) {
                const double ta = echoAt[j], da = echoDb[j], ka = echoDark[j];
                const int sa = echoSide[j], pa = echoPasses[j];
                echoAt[j] = echoAt[j - 1];
                echoDb[j] = echoDb[j - 1];
                echoDark[j] = echoDark[j - 1];
                echoSide[j] = echoSide[j - 1];
                echoPasses[j] = echoPasses[j - 1];
                echoAt[j - 1] = ta;
                echoDb[j - 1] = da;
                echoDark[j - 1] = ka;
                echoSide[j - 1] = sa;
                echoPasses[j - 1] = pa;
            }
    }

    // How many seconds the map shows: until the echoes fall 42 dB (3 to 12
    // of them), a whole bar when synced.
    double spanWanted() const {
        const double a0 = max(timeA0.get(), 0.001), b0 = max(timeB0.get(), 0.001);
        const double period = 0.5 * (a0 + b0);
        double n = 12.0;
        if (!frozenShown.get()) {
            const double g = feedback.value() * passGain(1000.0, period, rateNow());
            if (g < 0.999) n = g > 1e-6 ? 42.0 / max(-20.0 * log10(g), 0.1) + 1.0 : 1.0;
        }
        double s = clamp(n, 3.0, 12.0) * period + 0.25 * period;
        if (sync.index() > 0) {
            const double bar = 4.0 * 60.0 / bpm();
            s = ceil(s / bar) * bar;
        } else {
            s = s < 1.0 ? ceil(s * 4.0) / 4.0 : ceil(s);
        }
        return max(s, 0.1);
    }

    double timeX(double t) const { return mx + 18.0 + (mw - 36.0) * t / max(span, 0.05); }
    double stemHeight(double db) const {
        return (0.5 * mh - 26.0) * clamp((db + 48.0) / 48.0, 0.0, 1.0);
    }

    void drawMapGrid(Canvas& g) {
        const double axis = my + 0.5 * mh;
        g.lineWidth(1);
        if (sync.index() > 0) {
            const double beat = 60.0 / bpm();
            const int beats = min(ceilToInt(span / beat), 64);
            for (int k = 1; k <= beats; ++k) {
                const double x = timeX(k * beat);
                if (x > mx + mw - 4) break;
                const bool bar = imod(k, 4) == 0;
                g.strokeStyle(bar ? kGridLit : kGrid);
                g.beginPath();
                g.moveTo(x, my + 4);
                g.lineTo(x, my + mh - 4);
                g.stroke();
                if (bar || beats <= 16) {
                    g.fillStyle(bar ? Theme::textMuted : Theme::textFaint);
                    g.monoFont(8.5);
                    g.textAlign(TextAlign::Center);
                    g.textBaseline(TextBaseline::Middle);
                    if (bar)
                        g.fillText(format("bar %d", idiv(k, 4) + 1), x, my + mh - 9);
                    else
                        g.fillText(format("%d", k), x, my + mh - 9);
                }
            }
        } else {
            double step = 0.05;
            while ((mw - 36.0) * step / span < 56.0 && step < 10.0)
                step = step < 0.1 ? 0.1 : (step < 0.25 ? 0.25 : (step < 0.5 ? 0.5 : step * 2.0));
            const int count = min(floorToInt(span / step), 80);
            for (int k = 1; k <= count; ++k) {
                const double x = timeX(k * step);
                g.strokeStyle(kGrid);
                g.beginPath();
                g.moveTo(x, my + 4);
                g.lineTo(x, my + mh - 4);
                g.stroke();
                g.fillStyle(Theme::textFaint);
                g.monoFont(8.5);
                g.textAlign(TextAlign::Center);
                g.textBaseline(TextBaseline::Middle);
                fillSeconds(g, k * step, x, my + mh - 9);
            }
        }
        // The axis, and the -24 dB lines either side.
        g.strokeStyle(kGridLit);
        g.beginPath();
        g.moveTo(mx + 8, axis);
        g.lineTo(mx + mw - 8, axis);
        g.stroke();
        g.strokeStyle(kGrid);
        for (int s = -1; s <= 1; s += 2) {
            const double y = axis + s * stemHeight(-24.0);
            g.beginPath();
            g.moveTo(mx + 8, y);
            g.lineTo(mx + mw - 8, y);
            g.stroke();
        }
        label(g, "L", mx + 8, my + 12, TextAlign::Left);
        label(g, "R", mx + 8, my + mh - 24, TextAlign::Left);
    }

    void drawStem(Canvas& g, int i, double axis, double since) {
        const double x = timeX(echoAt[i]);
        if (x > mx + mw - 6) return;
        const double h = stemHeight(echoDb[i]);
        if (h < 1.0) return;
        const bool frozen = frozenShown.get();
        const bool lit = since >= echoAt[i] && since < echoAt[i] + 0.15;
        const int side = echoSide[i];
        for (int s = 0; s < 2; ++s) {
            if (side != 2 && side != s) continue;
            const double dir = s == 0 ? -1.0 : 1.0;
            const double tip = axis + dir * h;
            int colour = s == 0 ? kLinen : kFrond;
            if (frozen) colour = kFrost;
            // Diffusion smears each echo into the time after it, more each pass.
            const double smear = diffusion.value() * min(46.0, 7.0 + 5.0 * i);
            if (smear > 1.0 && !frozen) {
                g.fillStyle(colour);
                g.globalAlpha(0.13);
                g.beginPath();
                g.moveTo(x, tip);
                g.quadraticCurveTo(x + smear * 0.35, axis + dir * h * 0.25, x + smear, axis);
                g.lineTo(x, axis);
                g.closePath();
                g.fill();
            }
            // Reversed: a swell up to the echo.
            if (reverse.on()) {
                g.fillStyle(colour);
                g.globalAlpha(lit ? 0.5 : 0.25);
                g.beginPath();
                g.moveTo(x - min(26.0, 0.45 * (timeX(echoAt[0]) - timeX(0))), axis);
                g.lineTo(x, tip);
                g.lineTo(x, axis);
                g.closePath();
                g.fill();
            }
            // The stem browns as the loop darkens the echo.
            if (!frozen) colour = blend(colour, kCopper, echoDark[i]);
            g.globalAlpha(lit ? 1.0 : 0.9);
            g.strokeStyle(lit ? Theme::text : colour);
            g.lineWidth(lit ? 2.5 : 1.8);
            g.beginPath();
            g.moveTo(x, axis);
            g.lineTo(x, tip);
            g.stroke();
            g.globalAlpha(1.0);
            g.fillStyle(lit ? Theme::text : colour);
            g.beginPath();
            g.circle(x, tip, lit ? 4.5 : 3.2);
            g.fill();
        }
        // Each echo's pitch, when the loop shifts it.
        const double sh = shift.value();
        if (abs(sh) >= 0.05 && i < 8 && !frozen) {
            const double total = sh * echoPasses[i];
            if (abs(total) <= 36.0) {
                g.fillStyle(Theme::textMuted);
                g.monoFont(8.5);
                g.textAlign(TextAlign::Center);
                g.textBaseline(TextBaseline::Middle);
                const double y = side == 1 ? axis + h + 10 : axis - h - 10;
                if (abs(total - roundToInt(total)) < 0.05)
                    g.fillText(format("%+.0f", total), x, y);
                else
                    g.fillText(format("%+.1f", total), x, y);
            }
        }
    }

    void drawMap(Canvas& g) {
        panel(g, 16, 64, 748, 292);
        heading(g, "Echoes", 32, 84);
        aside(g, "drag across for the time, up and down for the feedback", 748, 84,
              TextAlign::Right);
        well(g, mx, my, mw, mh);
        g.save();
        g.clip(mx, my, mw, mh);
        drawMapGrid(g);
        const double axis = my + 0.5 * mh;
        const double since = sinceHitShown.get();
        // The sound itself, at 0.
        g.strokeStyle(Theme::textFaint);
        g.lineWidth(1.5);
        g.beginPath();
        g.moveTo(timeX(0), axis - stemHeight(0.0));
        g.lineTo(timeX(0), axis + stemHeight(0.0));
        g.stroke();
        for (int i = echoes - 1; i >= 0; --i) drawStem(g, i, axis, since);
        // The playhead, from the last sound that came in.
        if (since < span && !frozenShown.get()) {
            const double x = timeX(since);
            g.strokeStyle(kFrond);
            g.globalAlpha(0.6);
            g.lineWidth(1.5);
            g.beginPath();
            g.moveTo(x, my + 6);
            g.lineTo(x, my + mh - 18);
            g.stroke();
            g.globalAlpha(1.0);
        }
        if (frozenShown.get()) {
            g.fillStyle(kFrost);
            g.globalAlpha(0.06);
            g.fillRect(mx, my, mw, mh);
            g.globalAlpha(1.0);
            aside(g, "held: the loop repeats as it is, forever", mx + mw - 10, my + 14,
                  TextAlign::Right);
        }
        g.restore();
        drawMapFoot(g);
    }

    void drawMapFoot(Canvas& g) {
        const double y = my + mh + 18;
        g.fillStyle(Theme::textMuted);
        g.monoFont(10);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        const double t = echoTime.value() * 0.001;
        if (sync.index() > 0)
            g.fillText(format("every %.0f ms at %.1f BPM", t * 1000.0, bpm()), mx, y);
        else
            g.fillText(format("every %.0f ms", t * 1000.0), mx, y);
        g.textAlign(TextAlign::Right);
        const double d = decayShown.get();
        if (frozenShown.get())
            g.fillText("held", mx + mw, y);
        else if (d < 0.0)
            g.fillText("feedback past the loop's losses: the echoes grow", mx + mw, y);
        else if (feedback.value() <= 0.0)
            g.fillText("one echo", mx + mw, y);
        else if (d < 10.0)
            g.fillText(format("60 dB down in %.2f s", d), mx + mw, y);
        else
            g.fillText(format("60 dB down in %.1f s", d), mx + mw, y);
    }

    // --- the time panel -----------------------------------------------------------------------

    double gridCellX(int col) const { return gridX + col * (cellW + 4); }
    double gridCellY(int row) const { return gridY + row * (cellH + 4); }
    // The note in a cell of the grid: columns 1/16, 1/8, 1/4, 1/2; rows
    // straight, dotted, triplet; then 1/32, 1/1 and free.
    int cellNote(int row, int col) const {
        if (row == 3) return col == 0 ? 1 : (col == 1 ? 14 : 0);
        const int straight = col == 0 ? 3 : (col == 1 ? 6 : (col == 2 ? 9 : 12));
        return row == 0 ? straight : (row == 1 ? straight + 1 : straight - 1);
    }

    void drawTime(Canvas& g) {
        panel(g, 776, 64, 288, 292);
        heading(g, "Time", 792, 84);
        const bool synced = sync.index() > 0;
        // The time, in its own terms, and in the other's.
        g.fillStyle(Theme::text);
        g.serifFont(26);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Alphabetic);
        if (synced)
            fillNote(g, sync.index(), 792, 128);
        else
            fillSeconds(g, time.value() * 0.001, 792, 128);
        g.fillStyle(Theme::textMuted);
        g.monoFont(10);
        g.textBaseline(TextBaseline::Middle);
        if (synced) {
            g.fillText(format("%.0f ms at %.0f BPM", echoTime.value(), bpm()), 794, 143);
        } else {
            const int n = nearestNote(time.value() * 0.001);
            if (n > 0) {
                g.fillText("on", 794, 143);
                fillNote(g, n, 812, 143);
            } else {
                g.fillText(format("free, at %.0f BPM", bpm()), 794, 143);
            }
        }
        // How a change of time is reached.
        for (int k = 0; k < 2; ++k) {
            const double x = 946 + k * 56;
            pill(g, x, 104, 52, 20, k == 0 ? "BEND" : "FADE", timeChanges.index() == k, kLinen,
                 hovered(x, 104, 52, 20));
        }
        // The notes.
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col) {
                if (row == 3 && col == 3) continue;
                const int k = cellNote(row, col);
                const double x = gridCellX(col), y = gridCellY(row) + (row == 3 ? 4 : 0);
                const double w = row == 3 && col == 2 ? 2 * cellW + 4 : cellW;
                const bool lit = sync.index() == k;
                const bool hot = hovered(x, y, w, cellH);
                g.fillStyle(lit ? kFrond : (hot ? 0x3A2F27 : Theme::raised));
                g.beginPath();
                g.roundRect(x, y, w, cellH, 5);
                g.fill();
                if (!lit) {
                    g.strokeStyle(Theme::line);
                    g.lineWidth(1);
                    g.stroke();
                }
                g.fillStyle(lit ? kInk : Theme::textMuted);
                g.boldFont(9.5);
                g.textAlign(TextAlign::Center);
                g.textBaseline(TextBaseline::Middle);
                fillNote(g, k, x + w * 0.5, y + cellH * 0.5 + 0.5);
            }
        dial(g, knobX(0), knobY(0), knobR(0), time.normalized(), kLinen, false, hotKnob == 0,
             "TIME");
        dial(g, knobX(1), knobY(1), knobR(1), groove.normalized(), kFrond, true, hotKnob == 1,
             "GROOVE");
        dial(g, knobX(2), knobY(2), knobR(2), offset.normalized(), kFrond, true, hotKnob == 2,
             "OFFSET");
        valueStyle(g);
        g.globalAlpha(synced ? 0.45 : 1.0);
        fillSeconds(g, time.value() * 0.001, knobX(0), knobY(0) + 31);
        g.globalAlpha(1.0);
        if (abs(groove.value()) < 0.005)
            g.fillText("0%", knobX(1), knobY(1) + 31);
        else
            g.fillText(format("%+.0f%%", groove.value() * 100.0), knobX(1), knobY(1) + 31);
        if (abs(offset.value()) < 0.005)
            g.fillText("0%", knobX(2), knobY(2) + 31);
        else
            g.fillText(format("%+.0f%%", offset.value() * 100.0), knobX(2), knobY(2) + 31);
    }

    // --- the loop panel -----------------------------------------------------------------------

    void drawToneGraph(Canvas& g) {
        well(g, tgx, tgy, tgw, tgh);
        g.save();
        g.clip(tgx, tgy, tgw, tgh);
        g.frequencyGrid(tgx, tgy, tgw, tgh, 20, 20000);
        g.levelGrid(tgx, tgy, tgw, tgh, -48, 6, 12);
        const double seconds = 0.5 * (max(timeA0.get(), 0.001) + max(timeB0.get(), 0.001));
        const double r = rateNow();
        const double fb = frozenShown.get() ? 1.0 : feedback.value();
        for (int n = 3; n >= 0; --n) {
            const int k = n == 0 ? 1 : (n == 1 ? 2 : (n == 2 ? 4 : 8));
            g.strokeStyle(n == 0 ? kLinen : (n == 1 ? kFrond : (n == 2 ? kStraw : kCopper)));
            g.globalAlpha(1.0 - 0.18 * n);
            g.lineWidth(n == 0 ? 2.0 : 1.5);
            g.beginPath();
            for (int p = 0; p <= 120; ++p) {
                const double hz = 20.0 * pow(1000.0, p / 120.0);
                const double one = frozenShown.get() ? 1.0 : passGain(hz, seconds, r);
                double db = gainToDb(max(one, 1e-9)) * k + gainToDb(max(fb, 1e-9)) * (k - 1);
                if (fb <= 0.0 && k > 1) db = -200.0;
                const double y = dbToY(clamp(db, -60.0, 12.0), -48, 6, tgy, tgh);
                const double x = hzToX(hz, 20, 20000, tgx, tgw);
                if (p == 0)
                    g.moveTo(x, y);
                else
                    g.lineTo(x, y);
            }
            g.stroke();
        }
        g.globalAlpha(1.0);
        g.restore();
        // The legend: which echo is which.
        for (int n = 0; n < 4; ++n) {
            const double x = tgx + tgw - 150 + n * 36;
            g.strokeStyle(n == 0 ? kLinen : (n == 1 ? kFrond : (n == 2 ? kStraw : kCopper)));
            g.lineWidth(2);
            g.beginPath();
            g.moveTo(x, tgy + 12);
            g.lineTo(x + 12, tgy + 12);
            g.stroke();
            g.fillStyle(Theme::textMuted);
            g.monoFont(8.5);
            g.textAlign(TextAlign::Left);
            g.textBaseline(TextBaseline::Middle);
            g.fillText(n == 0 ? "1st" : (n == 1 ? "2nd" : (n == 2 ? "4th" : "8th")), x + 15,
                       tgy + 12);
        }
    }

    void drawLoop(Canvas& g) {
        panel(g, lx, py, lw, ph);
        heading(g, "Loop", lx + 16, py + 20);
        aside(g, "what every echo passes through", lx + lw - 16, py + 20, TextAlign::Right);
        dial(g, knobX(3), knobY(3), knobR(3), feedback.normalized(), kFrond, false, hotKnob == 3,
             "FEEDBACK");
        dial(g, knobX(4), knobY(4), knobR(4), lowCut.normalized(), kLinen, false, hotKnob == 4,
             "LOW CUT");
        dial(g, knobX(5), knobY(5), knobR(5), 1.0 - highCut.normalized(), kLinen, false,
             hotKnob == 5, "HIGH CUT");
        dial(g, knobX(6), knobY(6), knobR(6), diffusion.normalized(), kFrond, false, hotKnob == 6,
             "DIFFUSION");
        dial(g, knobX(7), knobY(7), knobR(7), shift.normalized(), kStraw, true, hotKnob == 7,
             "SHIFT");
        valueStyle(g);
        g.fillText(format("%.0f%%", feedback.value() * 100.0), knobX(3), knobY(3) + 50);
        if (lowCut < 20.5)
            g.fillText("off", knobX(4), knobY(4) + 33);
        else
            fillHz(g, lowCut, knobX(4), knobY(4) + 33);
        if (highCut >= 19500.0)
            g.fillText("off", knobX(5), knobY(5) + 33);
        else
            fillHz(g, highCut, knobX(5), knobY(5) + 33);
        g.fillText(format("%.0f%%", diffusion.value() * 100.0), knobX(6), knobY(6) + 33);
        if (abs(shift.value()) < 0.05)
            g.fillText("0 st", knobX(7), knobY(7) + 33);
        else
            g.fillText(format("%+.1f st", shift.value()), knobX(7), knobY(7) + 33);
        label(g, "EACH ECHO'S TONE", tgx, tgy - 12, TextAlign::Left);
        drawToneGraph(g);
        label(g, "ROUTING", tgx, py + 334, TextAlign::Left);
        for (int k = 0; k < 3; ++k) {
            const double x = tgx + k * 84;
            const char* name = k == 0 ? "STEREO" : (k == 1 ? "PING-PONG" : "MONO");
            pill(g, x, py + 344, 80, 22, name, routing.index() == k, kLinen,
                 hovered(x, py + 344, 80, 22));
        }
        label(g, "DIRECTION", tgx + 270, py + 334, TextAlign::Left);
        pill(g, tgx + 270, py + 344, 118, 22, reverse.on() ? "REVERSED" : "FORWARD", reverse.on(),
             kStraw, hovered(tgx + 270, py + 344, 118, 22));
    }

    // --- the medium panel ---------------------------------------------------------------------

    // Each medium's sign: a stepped wave, two tape reels, a row of buckets.
    void mediumGlyph(Canvas& g, int m, double x, double y, bool lit) {
        const int ink = lit ? kInk : mediumColour(m);
        g.strokeStyle(ink);
        g.fillStyle(ink);
        g.lineWidth(1.4);
        g.lineCap(LineCap::Round);
        g.lineJoin(LineJoin::Round);
        if (m == 0) {
            g.beginPath();
            for (int k = 0; k <= 12; ++k) {
                const double px = x + k * 2.5;
                const double v = y - 6.0 * sin(twoPi * k / 12.0);
                if (k == 0)
                    g.moveTo(px, v);
                else {
                    g.lineTo(px, y - 6.0 * sin(twoPi * (k - 1) / 12.0));
                    g.lineTo(px, v);
                }
            }
            g.stroke();
        } else if (m == 1) {
            for (int s = 0; s < 2; ++s) {
                const double rx = x + 7 + s * 16;
                g.beginPath();
                g.circle(rx, y - 2, 6);
                g.stroke();
                g.beginPath();
                g.circle(rx, y - 2, 1.6);
                g.fill();
            }
            g.beginPath();
            g.moveTo(x + 1, y + 4);
            g.lineTo(x + 29, y + 4);
            g.stroke();
        } else {
            for (int k = 0; k < 5; ++k) {
                const double bx = x + k * 6.5;
                g.beginPath();
                g.moveTo(bx, y - 6);
                g.lineTo(bx + 1, y + 2);
                g.lineTo(bx + 4, y + 2);
                g.lineTo(bx + 5, y - 6);
                g.stroke();
            }
        }
    }

    void drawMedium(Canvas& g) {
        panel(g, cx, py, cw, ph);
        heading(g, "Medium", cx + 16, py + 20);
        const int m = medium.index();
        for (int k = 0; k < 3; ++k) {
            const double x = cx + 16 + k * 92, y = py + 38;
            const bool lit = m == k;
            g.fillStyle(lit ? mediumColour(k) : (hovered(x, y, 84, 58) ? 0x3A2F27 : Theme::raised));
            g.beginPath();
            g.roundRect(x, y, 84, 58, 8);
            g.fill();
            if (!lit) {
                g.strokeStyle(Theme::line);
                g.lineWidth(1);
                g.stroke();
            }
            mediumGlyph(g, k, x + 27, y + 22, lit);
            g.fillStyle(lit ? kInk : Theme::textMuted);
            g.boldFont(9.5);
            g.textAlign(TextAlign::Center);
            g.textBaseline(TextBaseline::Middle);
            g.fillText(k == 0 ? "CLEAN" : (k == 1 ? "TAPE" : "ANALOG"), x + 42, y + 44);
        }
        dial(g, knobX(8), knobY(8), knobR(8), age.normalized(), mediumColour(m), false,
             hotKnob == 8, "AGE");
        dial(g, knobX(9), knobY(9), knobR(9), drive.normalized(), kCopper, false, hotKnob == 9,
             "DRIVE");
        valueStyle(g);
        g.fillText(format("%.0f%%", age.value() * 100.0), knobX(8), knobY(8) + 41);
        if (drive.value() < 0.05)
            g.fillText("clean", knobX(9), knobY(9) + 41);
        else
            g.fillText(format("%.1f dB", drive.value()), knobX(9), knobY(9) + 41);
        // What age does, in numbers.
        const double mid = cx + cw * 0.5;
        g.fillStyle(Theme::textMuted);
        g.monoFont(9.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        const double a = age.value();
        if (m == 0) {
            if (a < 0.02)
                g.fillText("full band, 24 bits", mid, py + 214);
            else
                g.fillText(format("band to %.1f kHz, %.1f bits", cleanHz(a) * 0.001, cleanBits(a)),
                           mid, py + 214);
        } else if (m == 1) {
            g.fillText(format("highs from %.1f kHz, wow and flutter", tapeHz(a) * 0.001), mid,
                       py + 214);
        } else {
            const double clock = bbdClock(0.5 * (timeA0.get() + timeB0.get()), a);
            g.fillText(format("clock %.1f kHz, band to %.1f kHz", clock * 0.001,
                              bbdHz(clock, rateNow()) * 0.001),
                       mid, py + 214);
        }
        aside(g,
              m == 0   ? "early digital: fewer bits, a narrower band"
              : m == 1 ? "worn tape: duller and less steady"
                       : "longer times darker, as a bucket brigade's",
              mid, py + 232, TextAlign::Center);
        label(g, "MODULATION", wvx, wvy - 12, TextAlign::Left);
        drawWave(g);
        dial(g, knobX(10), knobY(10), knobR(10), modulation.normalized(), mediumColour(m), false,
             hotKnob == 10, "DEPTH");
        dial(g, knobX(11), knobY(11), knobR(11), modRate.normalized(), mediumColour(m), false,
             hotKnob == 11, "RATE");
        valueStyle(g);
        g.fillText(format("%.1f ms", modulation.value() * kModReach * 1000.0), knobX(10),
                   knobY(10) + 32);
        if (modRate < 1.0)
            g.fillText(format("%.2f Hz", modRate.value()), knobX(11), knobY(11) + 32);
        else
            g.fillText(format("%.1f Hz", modRate.value()), knobX(11), knobY(11) + 32);
        aside(g, m == 1 ? "tape adds wow and flutter with age" : "how far the time wanders", wvx,
              wvy + wvh + 18, TextAlign::Left);
    }

    // The modulation's wave over two of its cycles, the dot where it is.
    void drawWave(Canvas& g) {
        well(g, wvx, wvy, wvw, wvh);
        const int m = medium.index();
        const double depth = modulation.value();
        const double mid = wvy + wvh * 0.5;
        g.strokeStyle(kGrid);
        g.lineWidth(1);
        g.beginPath();
        g.moveTo(wvx + 4, mid);
        g.lineTo(wvx + wvw - 4, mid);
        g.stroke();
        g.strokeStyle(mediumColour(m));
        g.lineWidth(1.6);
        g.globalAlpha(depth > 0.0 ? 1.0 : 0.35);
        g.beginPath();
        for (int p = 0; p <= 100; ++p) {
            const double ph2 = 2.0 * p / 100.0;
            const double ph1 = ph2 - floor(ph2);
            double v = 0.0;
            if (m == 2)
                v = ph1 < 0.25 ? 4.0 * ph1 : (ph1 < 0.75 ? 2.0 - 4.0 * ph1 : 4.0 * ph1 - 4.0);
            else
                v = sin(twoPi * ph1);
            const double y = mid - v * (wvh * 0.5 - 6) * max(depth, 0.15);
            const double x = wvx + 4 + (wvw - 8) * p / 100.0;
            if (p == 0)
                g.moveTo(x, y);
            else
                g.lineTo(x, y);
        }
        g.stroke();
        g.globalAlpha(1.0);
        if (depth > 0.0) {
            const double at = modPhaseShown.get();  // 0..1 across both cycles
            const double ph1 = 2.0 * at - floor(2.0 * at);
            double v = 0.0;
            if (m == 2)
                v = ph1 < 0.25 ? 4.0 * ph1 : (ph1 < 0.75 ? 2.0 - 4.0 * ph1 : 4.0 * ph1 - 4.0);
            else
                v = sin(twoPi * ph1);
            g.fillStyle(Theme::text);
            g.beginPath();
            g.circle(wvx + 4 + (wvw - 8) * at, mid - v * (wvh * 0.5 - 6) * depth, 3);
            g.fill();
        }
    }

    // --- the output panel ---------------------------------------------------------------------

    // The last four seconds: the input (faint), the echoes, and how far they
    // were ducked (shaded from the top).
    void drawTrail(Canvas& g) {
        well(g, trx, tryy, trw, trh);
        g.save();
        g.clip(trx, tryy, trw, trh);
        const int head = trailHead.get();
        for (int pass = 0; pass < 3; ++pass) {
            if (pass == 0) {
                g.fillStyle(kCopper);
                g.globalAlpha(0.35);
            } else {
                g.strokeStyle(pass == 1 ? Theme::textFaint : kFrond);
                g.lineWidth(pass == 1 ? 1.0 : 1.6);
                g.globalAlpha(1.0);
            }
            g.beginPath();
            if (pass == 0) g.moveTo(trx, tryy);
            for (int k = 0; k < kTrail; ++k) {
                const int at = wrap(head + k, kTrail);
                const double x = trx + trw * k / (kTrail - 1.0);
                double y = 0.0;
                if (pass == 0)
                    y = tryy + (trh - 4) * clamp(trailDuckShown[at] / 24.0, 0.0, 1.0);
                else {
                    const double db = pass == 1 ? trailInShown[at] : trailEchoShown[at];
                    y = tryy + trh - 3 - (trh - 8) * clamp((db + 60.0) / 60.0, 0.0, 1.0);
                }
                if (pass != 0 && k == 0)
                    g.moveTo(x, y);
                else
                    g.lineTo(x, y);
            }
            if (pass == 0) {
                g.lineTo(trx + trw, tryy);
                g.closePath();
                g.fill();
            } else {
                g.stroke();
            }
        }
        g.globalAlpha(1.0);
        g.restore();
        label(g, "DUCKING, LAST 4 S", trx + 8, tryy + 10, TextAlign::Left);
    }

    void drawOutput(Canvas& g) {
        panel(g, ox, py, ow, ph);
        heading(g, "Output", ox + 16, py + 20);
        aside(g, "ducking, width and level", ox + ow - 16, py + 20, TextAlign::Right);
        drawTrail(g);
        dial(g, knobX(12), knobY(12), knobR(12), duck.normalized(), kCopper, false, hotKnob == 12,
             "DUCK");
        dial(g, knobX(13), knobY(13), knobR(13), width.normalized(), kLinen, false, hotKnob == 13,
             "WIDTH");
        dial(g, knobX(14), knobY(14), knobR(14), input.normalized(), kLinen, false, hotKnob == 14,
             "INPUT");
        dial(g, knobX(15), knobY(15), knobR(15), mix.normalized(), kLinen, false, hotKnob == 15,
             "MIX");
        dial(g, knobX(16), knobY(16), knobR(16), output.normalized(), kLinen, true, hotKnob == 16,
             "OUTPUT");
        valueStyle(g);
        if (duck.value() < 0.05)
            g.fillText("off", knobX(12), knobY(12) + 32);
        else
            g.fillText(format("%.1f dB", duck.value()), knobX(12), knobY(12) + 32);
        g.fillText(format("%.0f%%", width.value() * 100.0), knobX(13), knobY(13) + 32);
        if (input.value() <= -47.99)
            g.fillText("closed", knobX(14), knobY(14) + 32);
        else
            fillDb(g, input.value(), knobX(14), knobY(14) + 32);
        g.fillText(format("%.0f%%", mix.value() * 100.0), knobX(15), knobY(15) + 40);
        fillDb(g, output.value(), knobX(16), knobY(16) + 40);
        const bool held = freeze.on();
        pill(g, ox + 16, py + 342, ow - 32, 26, held ? "HELD: CLICK TO LET GO" : "FREEZE", held,
             kFrost, hovered(ox + 16, py + 342, ow - 32, 26));
    }

    void draw(Canvas& g) {
        layoutEchoes();
        const double want = spanWanted();
        if (span <= 0.0 || dragging != 2) span = span <= 0.0 ? want : span + 0.25 * (want - span);
        g.linearGradient(0, 0, 0, kH, kGroundTop, kGroundBottom);
        g.fillRect(0, 0, kW, kH);
        drawHeader(g);
        drawMap(g);
        drawTime(g);
        drawLoop(g);
        drawMedium(g);
        drawOutput(g);
    }

    // --- the knobs ----------------------------------------------------------------------------

    double knobX(int k) const {
        switch (k) {
            case 0:
                return 828;
            case 1:
                return 920;
            case 2:
                return 1012;
            case 3:
                return 76;
            case 4:
                return 186;
            case 5:
                return 254;
            case 6:
                return 322;
            case 7:
                return 390;
            case 8:
                return 528;
            case 9:
                return 672;
            case 10:
                return 664;
            case 11:
                return 722;
            case 12:
                return 818;
            case 13:
                return 914;
            case 14:
                return 1010;
            case 15:
                return 850;
            default:
                return 976;
        }
    }
    double knobY(int k) const {
        if (k <= 2) return 300;
        if (k == 3) return 462;
        if (k <= 7) return 450;
        if (k <= 9) return 522;
        if (k <= 11) return 676;
        if (k <= 14) return 538;
        return 638;
    }
    double knobR(int k) const {
        if (k == 3) return 38.0;
        if (k == 8 || k == 9 || k == 15 || k == 16) return 28.0;
        if (k <= 2) return 19.0;
        return 21.0;
    }

    // --- the pointer --------------------------------------------------------------------------

    // how: 0 a value in its unit, 1 a knob position (0..1), 2 the default.
    void act(int how, const Param& p, double v) {
        if (how == 0)
            p.edit(v);
        else if (how == 1)
            p.editNormalized(v);
        else
            p.reset();
    }

    double knobValue(int k) const {
        switch (k) {
            case 0:
                return time.normalized();
            case 1:
                return groove.normalized();
            case 2:
                return offset.normalized();
            case 3:
                return feedback.normalized();
            case 4:
                return lowCut.normalized();
            case 5:
                // The high cut turns the other way: clockwise closes it.
                return 1.0 - highCut.normalized();
            case 6:
                return diffusion.normalized();
            case 7:
                return shift.normalized();
            case 8:
                return age.normalized();
            case 9:
                return drive.normalized();
            case 10:
                return modulation.normalized();
            case 11:
                return modRate.normalized();
            case 12:
                return duck.normalized();
            case 13:
                return width.normalized();
            case 14:
                return input.normalized();
            case 15:
                return mix.normalized();
            default:
                return output.normalized();
        }
    }

    void setKnob(int k, int how, double v) {
        if (k == 0) act(how, time, v);
        if (k == 1) act(how, groove, v);
        if (k == 2) act(how, offset, v);
        if (k == 3) act(how, feedback, v);
        if (k == 4) act(how, lowCut, v);
        if (k == 5) act(how, highCut, how == 1 ? 1.0 - v : v);
        if (k == 6) act(how, diffusion, v);
        if (k == 7) act(how, shift, v);
        if (k == 8) act(how, age, v);
        if (k == 9) act(how, drive, v);
        if (k == 10) act(how, modulation, v);
        if (k == 11) act(how, modRate, v);
        if (k == 12) act(how, duck, v);
        if (k == 13) act(how, width, v);
        if (k == 14) act(how, input, v);
        if (k == 15) act(how, mix, v);
        if (k == 16) act(how, output, v);
    }

    int knobAt(double x, double y) const {
        for (int k = 0; k < 17; ++k) {
            const double dx = x - knobX(k), dy = y - knobY(k);
            const double r = knobR(k) + 6;
            if (dx * dx + dy * dy <= r * r) return k;
        }
        return -1;
    }

    void pointer(const Pointer& p) {
        const PointerAction a = p.action();
        const double fine = p.shift() ? 0.2 : 1.0;
        if (a == PointerAction::Leave) {
            hoverX = -1.0;
            hoverY = -1.0;
            hotKnob = -1;
            return;
        }
        hoverX = p.x();
        hoverY = p.y();
        if (a == PointerAction::Move) {
            hotKnob = knobAt(p.x(), p.y());
            return;
        }
        if (a == PointerAction::Press) {
            pressed(p);
            return;
        }
        if (a == PointerAction::Drag) {
            if (dragging == 1) {
                knobPos = clamp(knobPos - p.dy() * fine / 220.0, 0.0, 1.0);
                setKnob(dragKnob, 1, knobPos);
            } else if (dragging == 2) {
                dragMap(p.x() - pressX, p.y() - pressY, fine);
            }
            return;
        }
        if (a == PointerAction::Release) {
            dragging = 0;
            hotKnob = knobAt(p.x(), p.y());
            return;
        }
        if (a == PointerAction::Wheel) {
            const double notches = p.wheel();
            if (p.over(mx, my, mw, mh)) {
                feedback.edit(feedback + notches * 0.01 * fine);
                return;
            }
            if (p.over(gridX, gridY, 4 * cellW + 12, 4 * cellH + 16) && sync.index() > 0) {
                const int n = clamp(lengthRank(sync.index()) + (notches > 0 ? 1 : -1), 0, 13);
                act(0, sync, noteByLength(n));
                return;
            }
            const int k = knobAt(p.x(), p.y());
            if (k >= 0) setKnob(k, 1, clamp(knobValue(k) + notches * 0.02 * fine, 0.0, 1.0));
        }
    }

    // Across: the time (free, by octaves; synced, a note value per 40
    // units); up and down: the feedback.
    void dragMap(double dx, double dy, double fine) {
        if (pressSync == 0) {
            time.edit(pressTime * exp2(dx * fine / 240.0));
        } else {
            const int n = clamp(lengthRank(pressSync) + roundToInt(dx * fine / 40.0), 0, 13);
            act(0, sync, noteByLength(n));
        }
        feedback.edit(pressFeedback - dy * fine / 260.0);
    }

    void pressed(const Pointer& p) {
        const double x = p.x(), y = p.y();
        const int k = knobAt(x, y);
        if (k >= 0) {
            if (p.doubleClick()) {
                setKnob(k, 2, 0);
                return;
            }
            dragging = 1;
            dragKnob = k;
            knobPos = knobValue(k);
            return;
        }
        if (p.over(mx, my, mw, mh)) {
            dragging = 2;
            pressX = x;
            pressY = y;
            pressTime = time.value();
            pressFeedback = feedback.value();
            pressSync = sync.index();
            return;
        }
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col) {
                if (row == 3 && col == 3) continue;
                const double cx0 = gridCellX(col), cy0 = gridCellY(row) + (row == 3 ? 4 : 0);
                const double w = row == 3 && col == 2 ? 2 * cellW + 4 : cellW;
                if (p.over(cx0, cy0, w, cellH)) {
                    const int note = cellNote(row, col);
                    // Free keeps the time the note had, so the echoes stay put.
                    if (note == 0 && sync.index() > 0) time.edit(echoTime.value());
                    act(0, sync, note);
                    return;
                }
            }
        for (int c = 0; c < 2; ++c)
            if (p.over(946 + c * 56, 104, 52, 20)) {
                act(0, timeChanges, c);
                return;
            }
        for (int c = 0; c < 3; ++c)
            if (p.over(tgx + c * 84, py + 344, 80, 22)) {
                act(0, routing, c);
                return;
            }
        if (p.over(tgx + 270, py + 344, 118, 22)) {
            act(0, reverse, reverse.on() ? 0 : 1);
            return;
        }
        for (int c = 0; c < 3; ++c)
            if (p.over(cx + 16 + c * 92, py + 38, 84, 58)) {
                act(0, medium, c);
                return;
            }
        if (p.over(ox + 16, py + 342, ow - 32, 26)) act(0, freeze, freeze.on() ? 0 : 1);
    }
};
