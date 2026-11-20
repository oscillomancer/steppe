// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Hickory: the stereo utility, the plain tool every chain needs now and then.
//
//   Polarity  either channel flipped, first, so a flipped pair is fixed
//             before anything sums it.
//   Channels  as they are, swapped, one side on both, the mono sum or the
//             side: to check a mix in mono or hear what is only in the side.
//   DC block  a 5 Hz high-pass, 12 dB/oct.
//   Delay     the whole signal, in whole samples, to line one track up with
//             another (a DI with its amp, a close mic with a room).
//   Width     the side against the mid; Bass Mono takes the side away below
//             a crossover with a Linkwitz-Riley high-pass on the side only,
//             so the mid, and the mono sum, are untouched.
//   Balance   one side turned down, the other left as it is.
//   Gain      last, with Mute.
//
// Every switch crossfades over 10 ms and every level glides, so all of it
// can be automated; with everything at rest the output is the input, bit
// for bit.
//
// The image shows where the sound sits (the mid up, the side across), with
// the correlation and the balance under it; width by frequency shows the
// correlation an octave at a time, which is where a wide bass or a flipped
// mic shows itself, and where Bass Mono's crossover is dragged.

constexpr int kBands = 10;     // octaves, 31 Hz to 16 kHz
constexpr int kPoints = 1024;  // the image's dots
constexpr int kChunks = 30;    // the meters' window: 30 chunks of 100 ms

// A value that moves to where it is sent along a straight line: the
// switches crossfade instead of clicking.
struct Ramp {
    double value = 0.0, target = 0.0, step = 0.0;
    int left = 0;

    void set(double v, int n, bool snap) {
        if (snap) {
            value = v;
            target = v;
            step = 0.0;
            left = 0;
            return;
        }
        if (v == target) return;
        target = v;
        left = max(1, n);
        step = (v - value) / left;
    }
    double next() {
        if (left > 0) {
            left -= 1;
            value = left == 0 ? target : value + step;
        }
        return value;
    }
};

// --- the look --------------------------------------------------------------------------

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
constexpr int kHickory = 0xD49A5B;  // the heartwood
constexpr int kLeaf = 0xA9B86C;     // the leaves: mono, in phase
constexpr int kRust = 0xC4673F;     // out of phase
constexpr int kBark = 0x6B5A48;

constexpr double kW = 900;
constexpr double kH = 580;

struct Hickory : Effect {
    static constexpr Info info = {
        .name = "Hickory",
        .id = "com.suppleaudio.hickory",
        .vendor = "Supple Audio",
        .version = "1.0.0",
        .description =
            "Stereo utility: gain, polarity, channel routing, width, balance, bass mono, DC "
            "block and a delay to line tracks up, with an image of where the sound sits.",
        .features = {"utility"}};

    Param gain = decibels("Gain", -60, 24, 0)
                     .modulated()
                     .offAtMin()
                     .smoothed(20)
                     .in("Level")
                     .help("The level out, last of all; the minimum is silence.");
    Param mute = toggle("Mute", false).in("Level").help("Silence, faded in and out over 10 ms.");
    Param delay =
        milliseconds("Delay", 0, 50, 0)
            .modulated()
            .in("Level")
            .help(
                "Delays the whole signal, to the nearest sample, to line it up with another "
                "track. It is not reported to the host as latency.");
    Param channels =
        choice("Channels", {"Stereo", "Swap", "Left", "Right", "Mono", "Side"}, 0)
            .keys({"stereo", "swap", "left", "right", "mono", "side"})
            .in("Stereo")
            .help(
                "What plays: the channels as they are, swapped, the left or the right on both, "
                "the mono sum (L+R)/2 on both, or the side (L-R)/2 on both.");
    Param width =
        percent("Width", 100, 0, 200)
            .modulated()
            .smoothed(20)
            .in("Stereo")
            .help("The side against the mid: 0% is mono, 100% as it is, 200% twice the side.");
    Param balance =
        percent("Balance", 0, -100, 100)
            .modulated()
            .smoothed(20)
            .in("Stereo")
            .help(
                "Turns one side down and leaves the other: positive lowers the left (+100% "
                "silences it), negative the right.");
    Param invertLeft = toggle("Invert Left", false)
                           .in("Stereo")
                           .help("Flips the left channel's polarity, before the routing.");
    Param invertRight = toggle("Invert Right", false)
                            .in("Stereo")
                            .help("Flips the right channel's polarity, before the routing.");
    Param bassMono =
        toggle("Bass Mono", false)
            .in("Low End")
            .help("Makes the lows mono: the side below Mono Below goes, the mid stays untouched.");
    Param monoBelow =
        frequency("Mono Below", 30, 500, 120)
            .modulated()
            .in("Low End")
            .activeWhen(bassMono)
            .help("Bass Mono's crossover: a 24 dB/oct Linkwitz-Riley high-pass on the side.");
    Param dcBlock = toggle("DC Block", false)
                        .in("Low End")
                        .help("Removes DC offset: a 5 Hz high-pass, 12 dB/oct.");

    Param outputPeak = decibels("Output Peak", -120, 24, -120)
                           .meter()
                           .help("The output's highest sample peak over the last 3 s, in dBFS.");
    Param correlation =
        continuous("Correlation", -1, 1, 0)
            .meter()
            .help(
                "The output's left-right correlation over the last 3 s: 1 is mono, 0 unrelated "
                "channels, -1 one side inverted; 0 in silence.");
    Param levelBalance = decibels("Level Balance", -24, 24, 0)
                             .meter()
                             .help(
                                 "How much louder the right channel is than the left over the "
                                 "last 3 s, in dB.");
    Param sideLevel =
        decibels("Side Level", -60, 12, -60)
            .meter()
            .help(
                "The side against the mid over the last 3 s, in dB: -60 is mono, 0 as much "
                "side as mid.");

    void factoryPatches(Patches& p) {
        p.add("Unity");
        p.add("Headroom").set(gain, -6);
        p.add("Mono Check").set(channels, "mono");
        p.add("Side Solo").set(channels, "side");
        p.add("Bass Mono").set(bassMono, true).set(monoBelow, 120);
        p.add("Wider, Mono Bass").set(width, 140).set(bassMono, true).set(monoBelow, 150);
        p.add("Narrower").set(width, 60);
        p.add("Flip Right").set(invertRight, true);
        p.add("DC Clean").set(dcBlock, true);
    }

    // --- audio ------------------------------------------------------------------------------

    double rate = 48000.0;
    int fade = 480;        // the switches' crossfade, 10 ms
    Array<Ramp, 4> route;  // left from L, left from R, right from L, right from R
    Ramp flipL, flipR, dcMix, monoMix, muted;
    Array<Biquad, 2> dc;
    Biquad sideA, sideB;
    Array<DelayLine, 2> line;
    int delayWant = 0, delayNow = 0, delayOld = 0;
    double delayMix = 1.0, delayStep = 0.001;
    double gainDb = 0.0, gainLinear = 1.0;
    bool unplayed = true;
    bool stereoBus = true;  // conservative until the first block supplies its layout

    // The meters: 3 s in chunks of 100 ms.
    int chunkLength = 4800, chunkAt = 0, chunkHead = 0, chunksFilled = 0;
    double cLL = 0.0, cRR = 0.0, cLR = 0.0, cMM = 0.0, cSS = 0.0, cPeak = 0.0;
    Array<double, 30> rLL, rRR, rLR, rMM, rSS, rPeak;
    double mPeak = -120.0, mCorrelation = 0.0, mBalance = 0.0, mSide = -60.0;

    // What the editor shows.
    Array<Biquad, 10> bandL, bandR;
    Array<double, 10> bLL, bRR, bLR;
    double bandAlpha = 0.0001, nowAlpha = 0.0001;
    double eLL = 0.0, eRR = 0.0, eLR = 0.0;
    int scopeStep = 4, scopeCount = 0, scopeAt = 0;
    double fall = 0.999;
    double pkInL = 0.0, pkInR = 0.0, pkOutL = 0.0, pkOutR = 0.0;

    SharedArray<float, 1024> scopeM, scopeS;
    SharedArray<float, 10> bandCorr, bandDb;
    Shared<int> scopeHead, delaySamples;
    Shared<double> inLeft, inRight, outLeft, outRight, corrNow, balanceNow;

    void prepare() {
        rate = sampleRate();
        fade = max(1, roundToInt(0.01 * rate));
        delayStep = 1.0 / max(1.0, 0.02 * rate);
        for (int c = 0; c < 2; ++c) {
            line[c].prepare(roundToInt(0.05 * rate) + 4);
            dc[c].highpass(5.0, 0.7071);
        }
        chunkLength = max(1, roundToInt(0.1 * rate));
        bandAlpha = 1.0 - exp(-1.0 / (0.25 * rate));
        nowAlpha = 1.0 - exp(-1.0 / (0.3 * rate));
        scopeStep = max(1, roundToInt(rate / 12000.0));
        fall = dbToGain(-24.0 / rate);  // the header's meters fall 24 dB a second
        for (int k = 0; k < kBands; ++k) {
            const double hz = min(31.25 * pow(2.0, k), 0.42 * rate);
            bandL[k].bandpass(hz, 1.414);
            bandR[k].bandpass(hz, 1.414);
        }
        unplayed = true;
        delayWant = clamp(roundToInt(delay * 0.001 * rate), 0, roundToInt(0.05 * rate));
        reportTail();
    }

    void update() {
        const bool snap = unplayed;
        Array<double, 4> m = {1.0, 0.0, 0.0, 1.0};
        const int mode = channels.index();
        if (mode == 1) m = {0.0, 1.0, 1.0, 0.0};
        if (mode == 2) m = {1.0, 0.0, 1.0, 0.0};
        if (mode == 3) m = {0.0, 1.0, 0.0, 1.0};
        if (mode == 4) m = {0.5, 0.5, 0.5, 0.5};
        if (mode == 5) m = {0.5, -0.5, 0.5, -0.5};
        for (int k = 0; k < 4; ++k) route[k].set(m[k], fade, snap);
        flipL.set(invertLeft.on() ? -1.0 : 1.0, fade, snap);
        flipR.set(invertRight.on() ? -1.0 : 1.0, fade, snap);
        dcMix.set(dcBlock.on() ? 1.0 : 0.0, fade, snap);
        monoMix.set(bassMono.on() ? 1.0 : 0.0, fade, snap);
        muted.set(mute.on() ? 0.0 : 1.0, fade, snap);
        if (snap || monoBelow.changed()) {
            const double hz = min(monoBelow.value(), 0.45 * rate);
            sideA.highpass(hz, 0.7071);
            sideB.highpass(hz, 0.7071);
        }
        delayWant = clamp(roundToInt(delay * 0.001 * rate), 0, roundToInt(0.05 * rate));
        if (snap) {
            delayNow = delayWant;
            delayOld = delayWant;
            delayMix = 1.0;
        }
        reportTail();
    }

    void reportTail() {
        // Keep both audible delay taps, and a pending change during a crossfade.
        int samples = max(delayWant, delayNow);
        if (delayMix < 1.0) samples = max(samples, delayOld);
        // At maximum gain and width, a full-scale impulse is below -100 dBFS
        // by 438 ms (5 Hz DC block) / 102 ms (30 Hz bass mono) at 8 kHz.
        // These bounds also leave margin for sustained input and filter cascades.
        double seconds = dcBlock.on() || dcMix.value != 0.0 ? 0.75 : 0.0;
        if (stereoBus && (bassMono.on() || monoMix.value != 0.0))
            seconds += 5.0 / min(monoBelow.value(), 0.45 * rate);
        setTail(samples + ceilToInt(seconds * rate));
    }

    // The chunk just ended joins the window, and the meters read the window.
    void closeChunk() {
        rLL[chunkHead] = cLL;
        rRR[chunkHead] = cRR;
        rLR[chunkHead] = cLR;
        rMM[chunkHead] = cMM;
        rSS[chunkHead] = cSS;
        rPeak[chunkHead] = cPeak;
        chunkHead = chunkHead + 1 == kChunks ? 0 : chunkHead + 1;
        chunksFilled = min(chunksFilled + 1, kChunks);
        cLL = 0.0;
        cRR = 0.0;
        cLR = 0.0;
        cMM = 0.0;
        cSS = 0.0;
        cPeak = 0.0;
        chunkAt = 0;
        double ll = 0.0, rr = 0.0, lr = 0.0, mm = 0.0, ss = 0.0, peak = 0.0;
        for (int k = 0; k < chunksFilled; ++k) {
            ll += rLL[k];
            rr += rRR[k];
            lr += rLR[k];
            mm += rMM[k];
            ss += rSS[k];
            peak = max(peak, rPeak[k]);
        }
        const double quiet = 1e-12 * chunksFilled * chunkLength;  // -120 dBFS
        mPeak = gainToDb(peak);
        mCorrelation = ll > quiet && rr > quiet ? clamp(lr / sqrt(ll * rr), -1.0, 1.0) : 0.0;
        mBalance = ll > quiet || rr > quiet
                       ? clamp(10.0 * log10((rr + 1e-30) / (ll + 1e-30)), -24.0, 24.0)
                       : 0.0;
        mSide = mm > quiet || ss > quiet
                    ? clamp(10.0 * log10((ss + 1e-30) / (mm + 1e-30)), -60.0, 12.0)
                    : -60.0;
    }

    void process(Block& io) {
        if (unplayed) unplayed = false;
        const bool stereo = io.channels > 1;
        stereoBus = stereo;
        for (int i = 0; i < io.frames; ++i) {
            const double inL = io[0][i];
            const double inR = stereo ? io[1][i] : inL;
            // Working values are finite: one bad sample never reaches a filter's state.
            const double l0 = isFinite(inL) ? inL : 0.0;
            const double r0 = isFinite(inR) ? inR : 0.0;
            pkInL = max(abs(l0), pkInL * fall);
            pkInR = max(abs(r0), pkInR * fall);
            bool neutral = true;

            // Polarity, then the routing.
            const double fl = flipL.next(), fr = flipR.next();
            double x = l0 * fl, y = r0 * fr;
            if (fl != 1.0 || fr != 1.0) neutral = false;
            if (stereo) {
                const double a = route[0].next(), b = route[1].next();
                const double c = route[2].next(), d = route[3].next();
                if (a != 1.0 || b != 0.0 || c != 0.0 || d != 1.0) {
                    const double l = x, r = y;
                    x = a * l + b * r;
                    y = c * l + d * r;
                    neutral = false;
                }
            }

            // DC block, its filters kept running so it fades in settled.
            const double dm = dcMix.next();
            const double hl = dc[0].process(x), hr = dc[1].process(y);
            if (dm != 0.0) {
                x += dm * (hl - x);
                y += dm * (hr - y);
                neutral = false;
            }

            // The delay, crossfading 20 ms from one time to the next.
            line[0].push(x);
            line[1].push(y);
            if (delayMix >= 1.0 && delayWant != delayNow) {
                delayOld = delayNow;
                delayNow = delayWant;
                delayMix = 0.0;
            }
            if (delayMix < 1.0) {
                delayMix = min(1.0, delayMix + delayStep);
                x = lerp(line[0].read(delayOld), line[0].read(delayNow), delayMix);
                y = lerp(line[1].read(delayOld), line[1].read(delayNow), delayMix);
                neutral = false;
            } else if (delayNow > 0) {
                x = line[0].read(delayNow);
                y = line[1].read(delayNow);
                neutral = false;
            }

            if (stereo) {
                // Width and bass mono, on the side.
                const double w = width.at(i), bm = monoMix.next();
                const double s0 = (x - y) * 0.5;
                const double low = sideB.process(sideA.process(s0));
                if (w != 1.0 || bm != 0.0) {
                    const double m = (x + y) * 0.5;
                    const double s = (s0 + bm * (low - s0)) * w;
                    x = m + s;
                    y = m - s;
                    neutral = false;
                }
                const double bal = balance.at(i);
                if (bal > 0.0) {
                    x *= 1.0 - bal;
                    neutral = false;
                } else if (bal < 0.0) {
                    y *= 1.0 + bal;
                    neutral = false;
                }
            }

            const double db = gain.at(i);
            // Reuse the exact conversion once the smoothed gain holds still.
            if (db != gainDb) {
                gainDb = db;
                gainLinear = db <= -60.0 ? 0.0 : dbToGain(db);
            }
            const double g = gainLinear * muted.next();
            if (g != 1.0) {
                x *= g;
                y *= g;
                neutral = false;
            }
            // At rest the input passes as it came, bit for bit.
            if (!neutral) {
                io[0][i] = x;
                if (stereo) io[1][i] = y;
            }
            if (!stereo) y = x;

            // What the meters and the editor read.
            pkOutL = max(abs(x), pkOutL * fall);
            pkOutR = max(abs(y), pkOutR * fall);
            const double mid = (x + y) * 0.5, side = (x - y) * 0.5;
            cLL += x * x;
            cRR += y * y;
            cLR += x * y;
            cMM += mid * mid;
            cSS += side * side;
            cPeak = max(cPeak, max(abs(x), abs(y)));
            chunkAt += 1;
            if (chunkAt >= chunkLength) closeChunk();
            eLL += nowAlpha * (x * x - eLL);
            eRR += nowAlpha * (y * y - eRR);
            eLR += nowAlpha * (x * y - eLR);
            for (int k = 0; k < kBands; ++k) {
                const double bl = bandL[k].process(x), br = bandR[k].process(y);
                bLL[k] += bandAlpha * (bl * bl - bLL[k]);
                bRR[k] += bandAlpha * (br * br - bRR[k]);
                bLR[k] += bandAlpha * (bl * br - bLR[k]);
            }
            scopeCount += 1;
            if (scopeCount >= scopeStep) {
                scopeCount = 0;
                scopeM[scopeAt] = static_cast<float>(mid);
                scopeS[scopeAt] = static_cast<float>(side);
                scopeAt = scopeAt + 1 == kPoints ? 0 : scopeAt + 1;
            }
        }

        reportTail();
        outputPeak.set(mPeak);
        correlation.set(mCorrelation);
        levelBalance.set(mBalance);
        sideLevel.set(mSide);
        for (int k = 0; k < kBands; ++k) {
            const double p = bLL[k] * bRR[k];
            bandCorr[k] = static_cast<float>(p > 1e-24 ? clamp(bLR[k] / sqrt(p), -1.0, 1.0) : 1.0);
            bandDb[k] = static_cast<float>(gainToDb(sqrt(bLL[k] + bRR[k])));
        }
        scopeHead.set(scopeAt);
        delaySamples.set(delayNow);
        inLeft.set(gainToDb(pkInL));
        inRight.set(gainToDb(pkInR));
        outLeft.set(gainToDb(pkOutL));
        outRight.set(gainToDb(pkOutR));
        const double p = eLL * eRR;
        corrNow.set(p > 1e-24 ? clamp(eLR / sqrt(p), -1.0, 1.0) : 0.0);
        balanceNow.set(eLL > 1e-12 || eRR > 1e-12
                           ? clamp(10.0 * log10((eRR + 1e-30) / (eLL + 1e-30)), -24.0, 24.0)
                           : 0.0);
    }

    // --- the editor's layout ------------------------------------------------------------------

    // The image.
    static constexpr double ix = 16, iy = 60, iw = 300, ih = 350;
    static constexpr double sx = 36, sy = 94, ss = 260;  // its well, square
    static constexpr double scx = sx + ss * 0.5, scy = sy + ss * 0.5;
    // Width by frequency.
    static constexpr double fx = 328, fy = 60, fw = 556, fh = 290;
    static constexpr double wx = 344, wy = 96, ww = 524, wh = 220;
    static constexpr double bassX = 760, bassY = 67;
    // Level, and Stereo.
    static constexpr double lx = 16, ly = 422, lw = 300, lh = 142;
    static constexpr double tx = 328, ty = 362, tw = 556, th = 202;
    static constexpr double pillY = ty + 36;

    // The editor's own.
    int dragging = 0;  // 1 a knob, 2 the crossover, 3 the image's width
    int dragKnob = -1;
    double knobPos = 0.0;
    int hotKnob = -1;
    double hoverX = -1.0, hoverY = -1.0;
    double zoom = 1.0;    // the image's scale, following the level
    bool zoomed = false;  // whether it has had a level to follow

    void editor(Editor& e) { e.size(kW, kH); }

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

    bool hovering(double x, double y, double w, double h) const {
        return hoverX >= x && hoverX <= x + w && hoverY >= y && hoverY <= y + h;
    }

    // A knob: an arc from 7 to 5 o'clock, lit from `from` (a position) to the value.
    void dial(Canvas& g, double x, double y, double r, double norm, double from, int colour,
              bool hot, const char* name) {
        const double start = 0.75 * pi, sweep = 1.5 * pi;
        g.lineCap(LineCap::Round);
        g.strokeStyle(kInk);
        g.lineWidth(r > 32 ? 6 : 5);
        g.beginPath();
        g.arc(x, y, r, start, start + sweep);
        g.stroke();
        const double at = start + sweep * clamp(norm, 0.0, 1.0);
        const double base = start + sweep * clamp(from, 0.0, 1.0);
        g.strokeStyle(colour);
        g.lineWidth(r > 32 ? 4.5 : 3.5);
        if (abs(at - base) > 0.001) {
            g.beginPath();
            if (at >= base)
                g.arc(x, y, r, base, at);
            else
                g.arc(x, y, r, at, base);
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

    void valueStyle(Canvas& g, double size) {
        g.fillStyle(Theme::text);
        g.monoFont(size);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
    }

    // The mark: a hickory leaf, its leaflets in pairs and one at the tip.
    void leaflet(Canvas& g, double x, double y, double angle, double len) {
        g.save();
        g.translate(x, y);
        g.rotate(angle);
        g.beginPath();
        g.moveTo(0, 0);
        g.quadraticCurveTo(len * 0.5, -len * 0.3, len, 0);
        g.quadraticCurveTo(len * 0.5, len * 0.3, 0, 0);
        g.fill();
        g.restore();
    }

    void drawMark(Canvas& g, double x, double y) {
        g.strokeStyle(kBark);
        g.lineCap(LineCap::Round);
        g.lineWidth(1.5);
        g.beginPath();
        g.moveTo(x - 8, y + 14);
        g.quadraticCurveTo(x - 3, y + 2, x + 4, y - 9);
        g.stroke();
        g.fillStyle(kLeaf);
        leaflet(g, x + 4, y - 9, -1.25, 11);
        Array<double, 2> along = {0.35, 0.68};
        for (int k = 0; k < 2; ++k) {
            const double u = along[k];
            const double bx = x - 8 + 12 * u, by = y + 14 - 23 * u;
            const double len = 8.0 + 2.0 * k;
            leaflet(g, bx, by, -2.35, len);
            leaflet(g, bx, by, -0.2, len);
        }
        g.fillStyle(kHickory);
        g.beginPath();
        g.circle(x - 7, y + 13, 2.6);
        g.fill();
    }

    // --- the header ---------------------------------------------------------------------------

    void drawLevel(Canvas& g, double x, double y, double w, double left, double right,
                   const char* name) {
        label(g, name, x - 8, y + 4, TextAlign::Right);
        for (int c = 0; c < 2; ++c) {
            const double db = c == 0 ? left : right;
            const double my = y + c * 6;
            g.fillStyle(kInk);
            g.beginPath();
            g.roundRect(x, my, w, 4, 2);
            g.fill();
            const double t = clamp((db + 60.0) / 60.0, 0.0, 1.0);
            if (t > 0.0) {
                g.linearGradient(x, 0, x + w, 0, kMeterLow, db > -0.1 ? Theme::alert : kMeterHigh);
                g.beginPath();
                g.roundRect(x, my, w * t, 4, 2);
                g.fill();
            }
        }
    }

    void drawHeader(Canvas& g) {
        drawMark(g, 31, 30);
        g.fillStyle(Theme::text);
        g.serifFont(23);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Alphabetic);
        g.fillText("Hickory", 52, 37);
        const double tw = g.measureText("Hickory");
        g.fillStyle(Theme::textMuted);
        g.serifItalicFont(13);
        g.fillText("stereo utility", 60 + tw, 37);
        g.textBaseline(TextBaseline::Middle);
        drawLevel(g, 520, 23, 140, inLeft.get(), inRight.get(), "IN");
        drawLevel(g, 724, 23, 140, outLeft.get(), outRight.get(), "OUT");
    }

    // --- the image ----------------------------------------------------------------------------

    // A bar from -1 to 1 (or a span in dB), lit from its middle to the value.
    void drawBar(Canvas& g, double y, double t, int colour, const char* name, const char* left,
                 const char* right) {
        const double bx = sx + 52, bw = ss - 52 - 74;
        label(g, name, sx, y, TextAlign::Left);
        label(g, left, bx - 6, y, TextAlign::Right);
        label(g, right, bx + bw + 6, y, TextAlign::Left);
        g.fillStyle(kInk);
        g.beginPath();
        g.roundRect(bx, y - 2.5, bw, 5, 2.5);
        g.fill();
        const double mid = bx + bw * 0.5, at = bx + bw * 0.5 * (1.0 + clamp(t, -1.0, 1.0));
        g.fillStyle(colour);
        g.beginPath();
        g.roundRect(min(mid, at), y - 2.5, abs(at - mid), 5, 2.5);
        g.fill();
        g.fillStyle(Theme::textMuted);
        g.fillRect(mid - 0.5, y - 5, 1, 10);
        g.fillStyle(Theme::text);
        g.beginPath();
        g.circle(at, y, 3.5);
        g.fill();
    }

    void drawImage(Canvas& g) {
        panel(g, ix, iy, iw, ih);
        heading(g, "Image", ix + 16, iy + 18);
        aside(g, "the mid up, the side across", ix + iw - 16, iy + 18, TextAlign::Right);
        well(g, sx, sy, ss, ss);
        const double r = ss * 0.5 - 14;
        g.save();
        g.clip(sx, sy, ss, ss);
        // The guides: the mid and the side, and the two channels on the diagonals.
        g.strokeStyle(kGrid);
        g.lineWidth(1);
        g.beginPath();
        g.circle(scx, scy, r);
        g.stroke();
        g.beginPath();
        g.circle(scx, scy, r * 0.5);
        g.stroke();
        g.strokeStyle(kGridLit);
        g.beginPath();
        g.moveTo(scx, scy - r);
        g.lineTo(scx, scy + r);
        g.moveTo(scx - r, scy);
        g.lineTo(scx + r, scy);
        g.stroke();
        g.strokeStyle(kGrid);
        const double d = r * 0.7071;
        g.beginPath();
        g.moveTo(scx - d, scy - d);
        g.lineTo(scx + d, scy + d);
        g.moveTo(scx + d, scy - d);
        g.lineTo(scx - d, scy + d);
        g.stroke();
        // The dots.
        // Scaled so the cloud's spread fills a third of the circle, and its
        // peaks never run far past it.
        double top = 0.0, power = 0.0;
        for (int k = 0; k < kPoints; ++k) {
            top = max(top, max(abs(scopeM[k] + scopeS[k]), abs(scopeM[k] - scopeS[k])));
            power += scopeM[k] * scopeM[k] + scopeS[k] * scopeS[k];
        }
        const double spread = sqrt(power / kPoints);
        const double want = top > 1e-5 ? min(0.4 / spread, 2.0 / top) : zoom;
        zoom = want < zoom || !zoomed ? want : zoom + 0.08 * (want - zoom);
        zoomed = top > 1e-5;
        zoom = clamp(zoom, 0.5, 3000.0);
        const int head = scopeHead.get();
        for (int half = 0; half < 2; ++half) {
            g.fillStyle(half == 0 ? kBark : kLinen);
            g.globalAlpha(half == 0 ? 0.55 : 0.6);
            g.beginPath();
            for (int j = 0; j < kPoints / 2; ++j) {
                const int k = wrap(head + half * (kPoints / 2) + j, kPoints);
                const double m = scopeM[k], s = scopeS[k];
                if (m == 0.0 && s == 0.0) continue;
                const double px = scx - s * zoom * r, py = scy - m * zoom * r;
                g.rect(px - 0.8, py - 0.8, 1.6, 1.6);
            }
            g.fill();
        }
        g.globalAlpha(1.0);
        g.restore();
        label(g, "M", scx, sy + 8, TextAlign::Center);
        label(g, "L", scx - d - 6, scy - d - 6, TextAlign::Center);
        label(g, "R", scx + d + 6, scy - d - 6, TextAlign::Center);
        label(g, "S", sx + 8, scy - 8, TextAlign::Left);
        if (top > 1e-5) {
            g.fillStyle(Theme::textFaint);
            g.monoFont(8.5);
            g.textAlign(TextAlign::Right);
            g.textBaseline(TextBaseline::Middle);
            g.fillText(format("%+.0f dB", gainToDb(zoom)), sx + ss - 8, sy + ss - 10);
        }
        // Under it: the correlation and the balance, as they are now.
        const double corr = corrNow.get();
        drawBar(g, sy + ss + 20, corr, corr < 0.0 ? kRust : kLeaf, "CORR", "-1", "+1");
        const double bal = balanceNow.get();
        drawBar(g, sy + ss + 42, bal / 12.0, kHickory, "LEVEL", "L", "R");
        valueStyle(g, 9.5);
        g.textAlign(TextAlign::Right);
        g.fillText(format("%+.2f", corr), ix + iw - 16, sy + ss + 20);
        if (abs(bal) < 0.05)
            g.fillText("0.0 dB", ix + iw - 16, sy + ss + 42);
        else
            g.fillText(format("%+.1f dB", bal), ix + iw - 16, sy + ss + 42);
        if (corr < -0.3 && top > 1e-4) {
            g.fillStyle(kRust);
            g.serifItalicFont(11);
            g.textAlign(TextAlign::Center);
            g.fillText("one side inverted? try Invert R", scx, sy + ss - 12);
        }
    }

    // --- width by frequency -------------------------------------------------------------------

    double bandHz(int k) const { return 31.25 * pow(2.0, k); }
    double freqX(double hz) const { return hzToX(hz, 20, 24000, wx, ww); }
    // The correlation's height: mono along the top, unrelated in the middle, inverted below.
    double corrY(double c) const { return wy + 24 + (wh - 40) * 0.5 * (1.0 - c); }
    double crossX() const { return freqX(monoBelow); }

    void drawWidth(Canvas& g) {
        panel(g, fx, fy, fw, fh);
        heading(g, "Width by frequency", fx + 16, fy + 18);
        const bool on = bassMono.on();
        pill(g, bassX, bassY, 108, 22, "BASS MONO", on, kLeaf, hovering(bassX, bassY, 108, 22));
        aside(g, "how alike the two sides are, an octave at a time", bassX - 12, fy + 18,
              TextAlign::Right);
        well(g, wx, wy, ww, wh);
        g.save();
        g.clip(wx, wy, ww, wh);
        const double xm = crossX();
        if (on) {
            g.fillStyle(kLeaf);
            g.globalAlpha(0.07);
            g.fillRect(wx, wy, xm - wx, wh);
            g.globalAlpha(1.0);
        }
        // The three levels, and the octaves.
        for (int k = 0; k < 3; ++k) {
            const double y = corrY(1.0 - k);
            g.strokeStyle(k == 1 ? kGridLit : kGrid);
            g.lineWidth(1);
            g.beginPath();
            g.moveTo(wx, y + 0.5);
            g.lineTo(wx + ww, y + 0.5);
            g.stroke();
        }
        g.fillStyle(kRust);
        g.globalAlpha(0.04);
        g.fillRect(wx, corrY(0.0), ww, corrY(-1.0) - corrY(0.0));
        g.globalAlpha(1.0);
        // A bar for each octave, from the mono line down: the longer, the wider.
        const double bw = ww / kBands * 0.56;
        for (int k = 0; k < kBands; ++k) {
            const double cx = freqX(bandHz(k));
            const double c = bandCorr[k], db = bandDb[k];
            const double alpha = clamp((db + 72.0) / 30.0, 0.0, 1.0);
            const double top = corrY(1.0), at = corrY(c), mid = corrY(0.0);
            g.fillStyle(kInk);
            g.globalAlpha(0.6);
            g.beginPath();
            g.roundRect(cx - bw * 0.5, top, bw, corrY(-1.0) - top, 3);
            g.fill();
            g.globalAlpha(1.0);
            if (alpha <= 0.0) continue;
            g.globalAlpha(0.25 + 0.75 * alpha);
            g.fillStyle(kHickory);
            g.beginPath();
            g.roundRect(cx - bw * 0.5, top, bw, max(2.0, min(at, mid) - top), 3);
            g.fill();
            if (c < 0.0) {
                g.fillStyle(kRust);
                g.beginPath();
                g.roundRect(cx - bw * 0.5, mid, bw, at - mid, 3);
                g.fill();
            }
            g.fillStyle(Theme::text);
            g.fillRect(cx - bw * 0.5, at - 1, bw, 2);
            g.globalAlpha(1.0);
        }
        // The crossover.
        g.strokeStyle(on ? kLeaf : Theme::textFaint);
        g.lineWidth(on ? 1.6 : 1.0);
        g.globalAlpha(on ? 1.0 : 0.6);
        if (on) {
            g.beginPath();
            g.moveTo(xm, wy);
            g.lineTo(xm, wy + wh);
            g.stroke();
        } else {
            for (int k = 0; k < 24; ++k) {
                g.beginPath();
                g.moveTo(xm, wy + k * 8.5);
                g.lineTo(xm, wy + k * 8.5 + 4);
                g.stroke();
            }
        }
        g.globalAlpha(1.0);
        g.fillStyle(on ? kLeaf : Theme::textFaint);
        g.boldFont(8.5);
        g.textBaseline(TextBaseline::Middle);
        g.textAlign(xm > wx + ww - 120 ? TextAlign::Right : TextAlign::Left);
        const double tx0 = xm > wx + ww - 120 ? xm - 6 : xm + 6;
        if (on)
            g.fillText(format("MONO BELOW %.0f HZ", monoBelow.value()), tx0, wy + 10);
        else
            g.fillText(format("%.0f HZ  DRAG TO MAKE THE LOWS MONO", monoBelow.value()), tx0,
                       wy + 10);
        g.restore();
        // The scales.
        label(g, "MONO", wx + 6, corrY(1.0) - 8, TextAlign::Left);
        label(g, "UNRELATED", wx + 6, corrY(0.0) - 8, TextAlign::Left);
        label(g, "INVERTED", wx + 6, corrY(-1.0) + 9, TextAlign::Left);
        g.fillStyle(Theme::textFaint);
        g.monoFont(8.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        for (int k = 0; k < kBands; ++k) {
            const double hz = bandHz(k);
            if (hz < 1000.0)
                g.fillText(format("%.0f Hz", hz), freqX(hz), wy + wh + 13);
            else
                g.fillText(format("%.0fk", hz / 1000.0), freqX(hz), wy + wh + 13);
        }
        // A hovered octave says what it holds.
        if (hovering(wx, wy, ww, wh) && dragging == 0) {
            int best = -1;
            for (int k = 0; k < kBands; ++k)
                if (abs(hoverX - freqX(bandHz(k))) < bw * 0.6) best = k;
            if (best >= 0) {
                const double hz = bandHz(best);
                valueStyle(g, 9.5);
                g.textAlign(TextAlign::Right);
                if (bandDb[best] < -90.0)
                    g.fillText(format("%.0f Hz  silent", hz), wx + ww - 8, wy + 10);
                else
                    g.fillText(format("%.0f Hz  %+.2f  %.0f dB", hz, bandCorr[best], bandDb[best]),
                               wx + ww - 8, wy + 10);
            }
        }
    }

    // --- the knobs and switches ---------------------------------------------------------------

    // 0 gain, 1 delay, 2 width, 3 balance, 4 mono below
    double knobX(int k) const {
        if (k == 0) return lx + 60;
        if (k == 1) return lx + 150;
        if (k == 2) return tx + 82;
        if (k == 3) return tx + 196;
        return tx + 480;
    }
    double knobY(int k) const { return k < 2 ? ly + 78 : ty + 128; }
    double knobR(int k) const { return k < 2 ? 30 : 34; }

    double knobValue(int k) const {
        if (k == 0) return gain.normalized();
        if (k == 1) return delay.normalized();
        if (k == 2) return width.normalized();
        if (k == 3) return balance.normalized();
        return monoBelow.normalized();
    }

    void drawKnobValue(Canvas& g, int k) {
        valueStyle(g, 10.5);
        const double x = knobX(k), y = knobY(k) + knobR(k) + 12;
        if (k == 0) {
            if (gain.value() <= -60.0)
                g.fillText("-inf dB", x, y);
            else if (abs(gain.value()) < 0.05)
                g.fillText("0.0 dB", x, y);
            else
                g.fillText(format("%+.1f dB", gain.value()), x, y);
        } else if (k == 1) {
            if (delay.value() < 0.005)
                g.fillText("0 ms", x, y);
            else
                g.fillText(format("%.2f ms", delay.value()), x, y);
        } else if (k == 2) {
            g.fillText(format("%.0f%%", width.value() * 100.0), x, y);
        } else if (k == 3) {
            const double b = balance.value() * 100.0;
            if (abs(b) < 0.5)
                g.fillText("centre", x, y);
            else if (b > 0.0)
                g.fillText(format("%.0f%% R", b), x, y);
            else
                g.fillText(format("%.0f%% L", -b), x, y);
        } else {
            g.fillText(format("%.0f Hz", monoBelow.value()), x, y);
        }
    }

    void drawLevelPanel(Canvas& g) {
        panel(g, lx, ly, lw, lh);
        heading(g, "Level", lx + 16, ly + 18);
        dial(g, knobX(0), knobY(0), knobR(0), gain.normalized(), 60.0 / 84.0, kHickory,
             hotKnob == 0, "GAIN");
        dial(g, knobX(1), knobY(1), knobR(1), delay.normalized(), 0.0, kHickory, hotKnob == 1,
             "DELAY");
        drawKnobValue(g, 0);
        drawKnobValue(g, 1);
        if (delay.value() >= 0.005) {
            g.fillStyle(Theme::textFaint);
            g.monoFont(8.5);
            g.fillText(format("%d samples", delaySamples.get()), knobX(1), knobY(1) + 55);
        }
        const double bx = lx + 210;
        pill(g, bx, ly + 52, 72, 24, "MUTE", mute.on(), Theme::alert,
             hovering(bx, ly + 52, 72, 24));
        pill(g, bx, ly + 90, 72, 24, "DC BLOCK", dcBlock.on(), kHickory,
             hovering(bx, ly + 90, 72, 24));
    }

    void drawStereoPanel(Canvas& g) {
        panel(g, tx, ty, tw, th);
        heading(g, "Stereo", tx + 16, ty + 18);
        aside(g, "polarity first, then the routing, width and balance", tx + tw - 16, ty + 18,
              TextAlign::Right);
        const int mode = channels.index();
        for (int k = 0; k < 6; ++k) {
            const double x = tx + 16 + k * 88;
            const char* name = "STEREO";
            if (k == 1) name = "SWAP";
            if (k == 2) name = "LEFT";
            if (k == 3) name = "RIGHT";
            if (k == 4) name = "MONO";
            if (k == 5) name = "SIDE";
            pill(g, x, pillY, 80, 24, name, mode == k, k == 0 ? kLeaf : kHickory,
                 hovering(x, pillY, 80, 24));
        }
        dial(g, knobX(2), knobY(2), knobR(2), width.normalized(), 0.5, kHickory, hotKnob == 2,
             "WIDTH");
        dial(g, knobX(3), knobY(3), knobR(3), balance.normalized(), 0.5, kHickory, hotKnob == 3,
             "BALANCE");
        dial(g, knobX(4), knobY(4), knobR(4), monoBelow.normalized(), 0.0,
             bassMono.on() ? kLeaf : kBark, hotKnob == 4, "MONO BELOW");
        for (int k = 2; k < 5; ++k) drawKnobValue(g, k);
        const double px = tx + 290;
        label(g, "POLARITY", px + 50, knobY(2) - knobR(2) - 12, TextAlign::Center);
        pill(g, px, knobY(2) - 30, 100, 24, "INVERT L", invertLeft.on(), kRust,
             hovering(px, knobY(2) - 30, 100, 24));
        pill(g, px, knobY(2) + 6, 100, 24, "INVERT R", invertRight.on(), kRust,
             hovering(px, knobY(2) + 6, 100, 24));
    }

    void draw(Canvas& g) {
        g.linearGradient(0, 0, 0, kH, kGroundTop, kGroundBottom);
        g.fillRect(0, 0, kW, kH);
        drawHeader(g);
        drawImage(g);
        drawWidth(g);
        drawLevelPanel(g);
        drawStereoPanel(g);
    }

    // --- the pointer ---------------------------------------------------------------------------

    // how: 1 a knob position (0..1), 2 the default.
    void setKnob(int k, int how, double v) {
        if (k == 0) {
            if (how == 1)
                gain.editNormalized(v);
            else
                gain.reset();
        }
        if (k == 1) {
            if (how == 1)
                delay.editNormalized(v);
            else
                delay.reset();
        }
        if (k == 2) {
            if (how == 1)
                width.editNormalized(v);
            else
                width.reset();
        }
        if (k == 3) {
            if (how == 1)
                balance.editNormalized(v);
            else
                balance.reset();
        }
        if (k == 4) {
            if (how == 1)
                monoBelow.editNormalized(v);
            else
                monoBelow.reset();
        }
    }

    int knobAt(double x, double y) const {
        for (int k = 0; k < 5; ++k) {
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
                monoBelow.edit(xToHz(clamp(p.x(), wx, wx + ww), 20, 24000, wx, ww));
            } else if (dragging == 3) {
                width.edit(width.value() + p.dx() * fine * 0.006);
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
            if (p.over(wx, wy, ww, wh)) {
                monoBelow.editNormalized(
                    clamp(monoBelow.normalized() + notches * 0.02 * fine, 0.0, 1.0));
                return;
            }
            if (p.over(sx, sy, ss, ss)) {
                width.edit(width.value() + notches * 0.05 * fine);
                return;
            }
            const int k = knobAt(p.x(), p.y());
            if (k >= 0) setKnob(k, 1, clamp(knobValue(k) + notches * 0.02 * fine, 0.0, 1.0));
        }
    }

    void toggleOf(const Param& t) { t.edit(t.on() ? 0 : 1); }

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
        // The crossover goes where the press is, and bass mono comes on; a
        // second press turns it off.
        if (p.over(wx, wy, ww, wh)) {
            if (p.doubleClick()) {
                bassMono.edit(0);
                return;
            }
            monoBelow.edit(xToHz(x, 20, 24000, wx, ww));
            bassMono.edit(1);
            dragging = 2;
            return;
        }
        // Across the image, the width stretches.
        if (p.over(sx, sy, ss, ss)) {
            if (p.doubleClick())
                width.reset();
            else
                dragging = 3;
            return;
        }
        if (p.over(bassX, bassY, 108, 22)) {
            toggleOf(bassMono);
            return;
        }
        for (int m = 0; m < 6; ++m)
            if (p.over(tx + 16 + m * 88, pillY, 80, 24)) {
                channels.edit(m);
                return;
            }
        const double px = tx + 290;
        if (p.over(px, knobY(2) - 30, 100, 24)) toggleOf(invertLeft);
        if (p.over(px, knobY(2) + 6, 100, 24)) toggleOf(invertRight);
        const double bx = lx + 210;
        if (p.over(bx, ly + 52, 72, 24)) toggleOf(mute);
        if (p.over(bx, ly + 90, 72, 24)) toggleOf(dcBlock);
    }
};
