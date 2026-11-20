// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Ghyll: a room to stand in, built from its walls.
//
// In a ghyll, a narrow ravine with a stream at its foot, sound comes back
// from both rock walls and the water's own noise hangs between them; in a
// hall it comes back from the walls, the floor and the ceiling. Ghyll is a
// room you build: its size in metres, what each surface is made of, where
// the sound stands and where you listen. Each part follows from the physics
// of a real room:
//
//   Reflections  the room's mirror images (every path off up to three
//                surfaces): each arrives as late as its path is long, as
//                weak as its distance, darkened by what it hit and by the
//                air it crossed, and picked up by the microphones (or a
//                head) as their pattern hears it from its direction. Rough
//                surfaces (scatter) spread part of each into a blur.
//   Decay        the reverberation time of each octave from the room's
//                volume, its surfaces' absorption and the air's (Eyring),
//                so a large hall darkens as it decays and a carpeted room
//                dies fastest in the highs.
//   The tail     sixteen delay lines as long as the room's mean free path,
//                mixed into one another and losing each octave's energy at
//                its rate. Its level is the room's: the reverberant energy
//                for this distance less what the reflections already
//                carry, so a far seat hears more room and a near one less.
//
// The dry signal is the direct sound; the mix at 50% is the balance heard
// from the listener's place.

constexpr double kSound = 343.0;  // m/s, at 20 C
constexpr int kImages = 62;       // the paths off one, two and three surfaces
constexpr int kLines = 16;        // the tail's delay lines
constexpr double kReach = 1.5;    // s: the longest reflection path, past the direct
constexpr double kPredelayLongest = 0.25;
constexpr double kLineLongest = 0.2;    // s: the longest delay line, modulation included
constexpr double kSourceQ = 1.92;       // the source's directivity: a voice, an instrument
constexpr double kHeadRadius = 0.0875;  // m
constexpr double kDensity = 0.02;       // s of mean line per second of decay, at the least

// --- the materials --------------------------------------------------------------------

// The share of the sound a surface absorbs in octave b (125, 250, 500 Hz, 1,
// 2, 4 kHz; the 8 kHz octave takes 4 kHz's), as found in rooms: hard surfaces
// take a little more than in a laboratory, for the doors, windows and
// fittings every real room has.
double absorption(int m, int b) {
    const int k = min(b, 5);
    switch (m) {
        case 0:  // stone, marble, glazed tile
            return k == 0 ? 0.03
                          : (k == 1 ? 0.03
                                    : (k == 2 ? 0.03 : (k == 3 ? 0.035 : (k == 4 ? 0.04 : 0.05))));
        case 1:  // concrete
            return k == 0 ? 0.04
                          : (k == 1 ? 0.04
                                    : (k == 2 ? 0.04 : (k == 3 ? 0.05 : (k == 4 ? 0.06 : 0.08))));
        case 2:  // brick
            return k == 0 ? 0.04
                          : (k == 1 ? 0.04
                                    : (k == 2 ? 0.045 : (k == 3 ? 0.05 : (k == 4 ? 0.06 : 0.08))));
        case 3:  // plaster
            return k == 0 ? 0.10
                          : (k == 1 ? 0.07
                                    : (k == 2 ? 0.05 : (k == 3 ? 0.045 : (k == 4 ? 0.045 : 0.05))));
        case 4:  // wood panels or boards
            return k == 0 ? 0.18
                          : (k == 1 ? 0.12
                                    : (k == 2 ? 0.10 : (k == 3 ? 0.08 : (k == 4 ? 0.07 : 0.07))));
        case 5:  // glass
            return k == 0 ? 0.20
                          : (k == 1 ? 0.08
                                    : (k == 2 ? 0.05 : (k == 3 ? 0.04 : (k == 4 ? 0.03 : 0.03))));
        case 6:  // carpet
            return k == 0 ? 0.02
                          : (k == 1 ? 0.06
                                    : (k == 2 ? 0.14 : (k == 3 ? 0.37 : (k == 4 ? 0.60 : 0.65))));
        case 7:  // heavy curtains
            return k == 0 ? 0.14
                          : (k == 1 ? 0.35
                                    : (k == 2 ? 0.55 : (k == 3 ? 0.72 : (k == 4 ? 0.70 : 0.65))));
        case 8:  // an audience in upholstered seats
            return k == 0 ? 0.60
                          : (k == 1 ? 0.74
                                    : (k == 2 ? 0.88 : (k == 3 ? 0.96 : (k == 4 ? 0.93 : 0.85))));
        case 9:  // acoustic panels
            return k == 0 ? 0.50
                          : (k == 1 ? 0.70
                                    : (k == 2 ? 0.80 : (k == 3 ? 0.90 : (k == 4 ? 0.90 : 0.85))));
        default:  // open: nothing there, the sound leaves
            return 1.0;
    }
}

// The air's absorption in octave b, dB per km (20 C, 50% humidity).
double airDb(int b) {
    switch (b) {
        case 0:
            return 0.44;
        case 1:
            return 1.31;
        case 2:
            return 2.73;
        case 3:
            return 4.66;
        case 4:
            return 9.86;
        case 5:
            return 32.8;
        default:
            return 117.0;
    }
}
// As an energy loss per metre.
double airPerMetre(int b) { return airDb(b) * 0.001 / 4.3429448190325; }

double octaveHz(int b) { return 125.0 * exp2(b); }

// The reflections' three bands (below 500 Hz, 500 Hz to 4 kHz, above): what
// a surface absorbs and the air takes in each.
double bandAbsorption(int m, int band) {
    if (band == 0) return 0.5 * (absorption(m, 0) + absorption(m, 1));
    if (band == 1) return (absorption(m, 2) + absorption(m, 3) + absorption(m, 4)) / 3.0;
    return absorption(m, 5);
}
double bandAir(int band) {
    if (band == 0) return 0.5 * (airPerMetre(0) + airPerMetre(1));
    if (band == 1) return airPerMetre(3);
    return sqrt(airPerMetre(5) * airPerMetre(6));
}
double bandHz(int band) { return band == 0 ? 200.0 : (band == 1 ? 1300.0 : 6500.0); }

// A cardioid's pattern, for a sound arriving along u (from the capsule
// toward where it comes from) at a capsule facing a (level: a has no height).
double cardioid(double ux, double uy, double ax, double ay) {
    return 0.5 + 0.5 * (ux * ax + uy * ay);
}
// A head's shadow on one ear (a sphere): the gain at hz for a sound at an
// angle theta (radians) from the ear's side.
double headShadow(double theta, double hz) {
    const double alpha = 1.05 + 0.95 * cos(theta * 1.2);
    const double w = twoPi * hz * kHeadRadius / (2.0 * kSound);
    return sqrt((1.0 + alpha * alpha * w * w) / (1.0 + w * w));
}

// --- the room ------------------------------------------------------------------------

// Everything that follows from the room as set: the source's images and
// what each microphone hears of them, each octave's decay, and the levels.
// The audio code keeps one and the editor one of its own.
struct Room {
    // As set.
    double w = 17.0, d = 25.0, h = 11.0;
    int walls = 3, floor = 8, ceiling = 4, mic = 0;
    double scale = 1.0, scatter = 0.4, predelay = 0.0, rate = 48000.0;
    double sxp = 0.44, syp = 0.22, lxp = 0.54, lyp = 0.44;

    // Where things are, metres: x across from the left wall, y back from
    // the front wall, z up from the floor.
    double sx = 0.0, sy = 0.0, sz = 1.5, lx = 0.0, ly = 0.0, lz = 1.7;
    double fx = 0.0, fy = -1.0;       // the listener faces the source
    double r0 = 1.0;                  // source to listener
    Array<double, 2> cx, cy, ax, ay;  // the capsules (or ears): where, and facing

    // The room's acoustics.
    double volume = 1.0, surface = 1.0, freePath = 1.0;  // m3, m2, m
    Array<double, 7> rt;                                 // s, per octave
    double absorptionMid = 1.0;                          // m2 (Eyring, the air's too)
    double critical = 1.0;                               // m
    double first = 0.0;                                  // s: the first reflection, past the direct
    double firstOrder = 0.0;                             // s: the first-order reflections' mean
    double reverberant = 1.0, early = 0.0;               // energies against the direct sound's
    double lateGain = 0.0, sideHz = 200.0;
    double lateOmni = 0.0;  // the tail's energy as an omni hears it

    // The room's modes, below the Schroeder frequency (where they stand
    // apart): each a standing wave, heard as strongly as the source and the
    // listener sit near its peaks.
    Array<double, 48> modeHz, modeDecay;  // Hz; the amplitude's decay, per second
    Array<double, 96> modeGain;           // what each microphone hears of it (m * 48 + n)
    int modes = 0;
    double crossover = 100.0;  // Hz: the modes below it, the tail above

    // The images.
    Array<double, 62> ix, iy, iz;  // where
    Array<int, 62> order, onWalls, onFloor, onCeiling;
    Array<double, 124> delay;   // s, per microphone (m * 62 + i), predelay included
    Array<double, 372> spec;    // each band's gain, ((m * 62 + i) * 3 + band): the clear part
    Array<double, 372> blur;    // and the part scatter spreads
    Array<double, 62> shownDb;  // the image's level as an omni hears it, mid band
    int images = 0;

    double reflect(int m, int band) const {
        return sqrt(max(0.0, 1.0 - min(bandAbsorption(m, band), 1.0)));
    }

    void place() {
        const double margin = 0.25;
        sx = clamp(sxp * w, margin, w - margin);
        sy = clamp(syp * d, margin, d - margin);
        lx = clamp(lxp * w, margin, w - margin);
        ly = clamp(lyp * d, margin, d - margin);
        sz = clamp(1.5, 0.3, h - 0.3);
        lz = clamp(1.7, 0.3, h - 0.3);
        double dx = sx - lx, dy = sy - ly;
        double flat = sqrt(dx * dx + dy * dy);
        const double horizontal = flat;  // keep the distance when a fallback facing is needed
        if (flat < 1e-6) {
            dx = 0.0;
            dy = -1.0;
            flat = 1.0;
        }
        fx = dx / flat;
        fy = dy / flat;
        const double dz = sz - lz;
        r0 = max(0.5, sqrt(horizontal * horizontal + dz * dz));
        // The listener's right, facing the source.
        const double rx = -fy, ry = fx;
        double half = 0.085, angle = 0.9599310885968813;  // ORTF: 17 cm, 55 degrees out
        if (mic == 1) half = 0.3;                         // spaced omnis, 60 cm
        if (mic == 2) half = kHeadRadius;                 // ears
        if (mic == 3) half = 0.0;
        if (mic == 2) angle = 0.5 * pi;  // an ear faces sideways
        for (int m = 0; m < 2; ++m) {
            const double s = m == 0 ? -1.0 : 1.0;
            cx[m] = lx + s * rx * half;
            cy[m] = ly + s * ry * half;
            ax[m] = cos(angle) * fx + s * sin(angle) * rx;
            ay[m] = cos(angle) * fy + s * sin(angle) * ry;
        }
    }

    // What capsule m hears, in a band, of a sound arriving along u (a unit
    // vector; only its level part counts, as the capsules face level).
    double pattern(int m, double ux, double uy, int band) const {
        if (mic == 0) return cardioid(ux, uy, ax[m], ay[m]);
        if (mic == 2) {
            const double c = clamp(ux * ax[m] + uy * ay[m], -1.0, 1.0);
            return headShadow(acos(c), bandHz(band));
        }
        return 1.0;
    }

    void acoustics() {
        volume = w * d * h;
        const double sw = 2.0 * h * (w + d), sf = w * d;
        surface = sw + 2.0 * sf;
        freePath = 4.0 * volume / surface;
        for (int b = 0; b < 7; ++b) {
            const double a = (sw * absorption(walls, b) + sf * absorption(floor, b) +
                              sf * absorption(ceiling, b)) /
                             surface;
            const double area =
                (-surface * log(max(1e-9, 1.0 - min(a, 0.9999))) + 4.0 * airPerMetre(b) * volume) /
                scale;
            rt[b] = 0.161 * volume / area;
            if (b == 3) absorptionMid = area;
        }
        critical = sqrt(kSourceQ * absorptionMid / (16.0 * pi));
    }

    // The source's pattern for a path leaving along (ex, ey, ez), against its
    // pattern toward the listener: wider than a cardioid, as a voice is.
    // (fx, fy) faces from listener to source; the source faces the other way.
    double sourceGain(double ex, double ey, double ez) const {
        const double n = sqrt(ex * ex + ey * ey + ez * ez);
        if (n < 1e-9) return 1.0;
        return 0.7 - 0.3 * (ex * fx + ey * fy) / n;
    }

    void compute() {
        place();
        acoustics();
        // The direct sound as each capsule hears it, which the dry signal is.
        Array<double, 6> direct;
        for (int m = 0; m < 2; ++m) {
            const double vx = sx - cx[m], vy = sy - cy[m], vz = sz - lz;
            const double r = max(0.05, sqrt(vx * vx + vy * vy + vz * vz));
            for (int band = 0; band < 3; ++band)
                direct[m * 3 + band] = max(0.25, pattern(m, vx / r, vy / r, band));
        }
        const double invScale = 1.0 / scale;
        images = 0;
        early = 0.0;
        first = kReach;
        firstOrder = 0.0;
        for (int nx = -1; nx <= 2; ++nx)
            for (int px = 0; px <= 1; ++px)
                for (int ny = -1; ny <= 2; ++ny)
                    for (int py = 0; py <= 1; ++py)
                        for (int nz = -1; nz <= 2; ++nz)
                            for (int pz = 0; pz <= 1; ++pz) {
                                const int hx = abs(nx - px) + abs(nx), hy = abs(ny - py) + abs(ny);
                                const int hz = abs(nz - pz) + abs(nz);
                                const int k = hx + hy + hz;
                                if (k == 0 || k > 3 || images >= kImages) continue;
                                const int i = images;
                                images += 1;
                                ix[i] = (1 - 2 * px) * sx + 2.0 * nx * w;
                                iy[i] = (1 - 2 * py) * sy + 2.0 * ny * d;
                                iz[i] = (1 - 2 * pz) * sz + 2.0 * nz * h;
                                order[i] = k;
                                onWalls[i] = hx + hy;
                                onFloor[i] = abs(nz - pz);
                                onCeiling[i] = abs(nz);
                                image(i, px, py, pz, direct, invScale);
                            }
        // The rest of the room's energy is the tail's: what the room holds
        // for this distance (Sabine's reverberant field, the source's
        // directivity counted) less the reflections'.
        reverberant = 16.0 * pi * r0 * r0 / (kSourceQ * absorptionMid);
        double late = max(reverberant - early, 0.25 * reverberant);
        lateOmni = late;
        // A cardioid hears a third of a diffuse field's energy; a head about
        // all of it. Against the direct sound each hears.
        if (mic == 0) late *= (1.0 / 3.0) / (direct[1] * direct[1]);
        if (mic == 2) late *= 1.0 / (direct[1] * direct[1]);
        lateGain = sqrt(late);
        sideHz = mic == 0 ? 250.0 : (mic == 1 ? 150.0 : 500.0);
        standingWaves(late);
    }

    // The modes up to the Schroeder frequency (30 to 250 Hz), the lowest 48.
    // Each rings at its frequency and decays at the rate the surfaces it
    // meets allow (a mode along one axis meets the other walls only at
    // grazing incidence, and outlasts the rest). Their level is the tail's
    // below the crossover, shared out by the density of modes there.
    void standingWaves(double late) {
        double fx = clamp(2000.0 * sqrt(rt[0] / volume), 30.0, 250.0);
        for (int round = 0; round < 16 && countModes(fx) > 48; ++round) fx *= 0.92;
        crossover = fx;
        const double ax = 2.0 * d * h * absorption(walls, 0),
                     ay = 2.0 * w * h * absorption(walls, 0);
        const double az = w * d * (absorption(floor, 0) + absorption(ceiling, 0));
        const double areas = max(1e-6, ax + ay + az);
        const double edges = 4.0 * (w + d + h);
        modes = 0;
        double spread = 0.0;
        const int nxTop = floorToInt(2.0 * w * fx / kSound),
                  nyTop = floorToInt(2.0 * d * fx / kSound);
        const int nzTop = floorToInt(2.0 * h * fx / kSound);
        for (int nx = 0; nx <= nxTop; ++nx)
            for (int ny = 0; ny <= nyTop; ++ny)
                for (int nz = 0; nz <= nzTop; ++nz) {
                    if (nx + ny + nz == 0 || modes >= 48) continue;
                    const double kx = nx / w, ky = ny / d, kz = nz / h;
                    const double f = 0.5 * kSound * sqrt(kx * kx + ky * ky + kz * kz);
                    if (f > fx) continue;
                    const int n = modes;
                    modes += 1;
                    modeHz[n] = f;
                    const double weight = ((nx > 0 ? 2.0 : 1.0) * ax + (ny > 0 ? 2.0 : 1.0) * ay +
                                           (nz > 0 ? 2.0 : 1.0) * az) /
                                          (2.0 * areas);
                    modeDecay[n] = 6.907755278982137 / rtAt(f) * weight;
                    const double density = 4.0 * pi * volume * f * f / (kSound * kSound * kSound) +
                                           pi * surface * f / (2.0 * kSound * kSound) +
                                           edges / (8.0 * kSound);
                    const double norm =
                        (nx > 0 ? 0.5 : 1.0) * (ny > 0 ? 0.5 : 1.0) * (nz > 0 ? 0.5 : 1.0);
                    const double atSource =
                        cos(pi * nx * sx / w) * cos(pi * ny * sy / d) * cos(pi * nz * sz / h);
                    for (int m = 0; m < 2; ++m) {
                        const double atMic = cos(pi * nx * cx[m] / w) * cos(pi * ny * cy[m] / d) *
                                             cos(pi * nz * lz / h);
                        modeGain[m * 48 + n] = atSource * atMic / norm / sqrt(density);
                    }
                    // What it rings for, on average over the places in the room,
                    // per unit of the gain shared out below: sum of (sin decaying)^2.
                    spread += 1.0 / (4.0 * modeDecay[n] * density);
                }
        // The tail's energy below the crossover (an impulse's is spread evenly
        // to half the sample rate), at the lows' decay. A mode of gain a rings
        // a^2 rate / (4 decay) of it.
        const double want = late * rt[0] / max(rtMid(), 1e-6) * fx / (0.5 * rate);
        const double scaleGain = spread > 0.0 ? sqrt(want / (spread * rate)) : 0.0;
        for (int k = 0; k < 96; ++k) modeGain[k] *= scaleGain;
    }

    int countModes(double fx) const {
        int count = 0;
        const int nxTop = floorToInt(2.0 * w * fx / kSound),
                  nyTop = floorToInt(2.0 * d * fx / kSound);
        const int nzTop = floorToInt(2.0 * h * fx / kSound);
        for (int nx = 0; nx <= nxTop && count <= 48; ++nx)
            for (int ny = 0; ny <= nyTop && count <= 48; ++ny)
                for (int nz = 0; nz <= nzTop; ++nz) {
                    const double kx = nx / w, ky = ny / d, kz = nz / h;
                    if (nx + ny + nz > 0 && 0.5 * kSound * sqrt(kx * kx + ky * ky + kz * kz) <= fx)
                        count += 1;
                }
        return count;
    }

    // The reverberation time at f, between the octaves (log frequency).
    double rtAt(double f) const {
        if (f <= 125.0) return rt[0];
        const double at = clamp(log2(f / 125.0), 0.0, 5.999);
        const int b = floorToInt(at);
        return rt[b] + (at - b) * (rt[b + 1] - rt[b]);
    }

    void image(int i, int px, int py, int pz, const Array<double, 6>& direct, double invScale) {
        const int k = order[i];
        // Each surface it met, per band; the air on the way.
        Array<double, 3> surfaces;
        for (int band = 0; band < 3; ++band) {
            const double r = pow(reflect(walls, band), onWalls[i]) *
                             pow(reflect(floor, band), onFloor[i]) *
                             pow(reflect(ceiling, band), onCeiling[i]);
            surfaces[band] = pow(r, invScale);
        }
        // Leaving the source: the image's path, folded back by its mirrors.
        const double ex = (lx - ix[i]) * (1 - 2 * px), ey = (ly - iy[i]) * (1 - 2 * py);
        const double ez = (lz - iz[i]) * (1 - 2 * pz);
        const double emitted = sourceGain(ex, ey, ez);
        // Scatter takes more of the highs than the lows.
        Array<double, 3> clear, spread;
        for (int band = 0; band < 3; ++band) {
            const double s = scatter * (band == 0 ? 0.3 : (band == 1 ? 0.75 : 1.0));
            const double kept = pow(1.0 - s, k);
            clear[band] = sqrt(kept);
            spread[band] = sqrt(1.0 - kept);
        }
        // As an omni at the listener hears it: the shown level and the energy.
        {
            const double vx = ix[i] - lx, vy = iy[i] - ly, vz = iz[i] - lz;
            const double r = sqrt(vx * vx + vy * vy + vz * vz);
            const double a =
                r0 / r * surfaces[1] * exp(-0.5 * bandAir(1) * (r - r0) * invScale) * emitted;
            early += a * a;
            shownDb[i] = gainToDb(a);
        }
        for (int m = 0; m < 2; ++m) {
            const double vx = ix[i] - cx[m], vy = iy[i] - cy[m], vz = iz[i] - lz;
            const double r = max(0.05, sqrt(vx * vx + vy * vy + vz * vz));
            const double late = max(0.0002, (r - r0) / kSound);
            first = min(first, late);
            if (m == 0 && k == 1) firstOrder += late / 6.0;
            delay[m * 62 + i] = min(late, kReach) + predelay;
            for (int band = 0; band < 3; ++band) {
                const double heard = pattern(m, vx / r, vy / r, band) / direct[m * 3 + band];
                const double a = r0 / r * surfaces[band] *
                                 exp(-0.5 * bandAir(band) * (r - r0) * invScale) * emitted * heard;
                spec[(m * 62 + i) * 3 + band] = a * clear[band];
                blur[(m * 62 + i) * 3 + band] = a * spread[band];
            }
        }
    }

    // An image's path through the room, from the source to the listener:
    // its points (at most five: the two ends and three bounces) into px,
    // py, pz; how many. The straight line from the listener to the image,
    // folded back into the room at each wall it crosses.
    int path(int i, Array<double, 5>& px, Array<double, 5>& py, Array<double, 5>& pz) const {
        Array<double, 3> at;
        int n = 0;
        const double dx = ix[i] - lx, dy = iy[i] - ly, dz = iz[i] - lz;
        for (int axis = 0; axis < 3; ++axis) {
            const double from = axis == 0 ? lx : (axis == 1 ? ly : lz);
            const double to = axis == 0 ? ix[i] : (axis == 1 ? iy[i] : iz[i]);
            const double size = axis == 0 ? w : (axis == 1 ? d : h);
            const double step = axis == 0 ? dx : (axis == 1 ? dy : dz);
            if (abs(step) < 1e-9) continue;
            const int lo = floorToInt(min(from, to) / size), hi = ceilToInt(max(from, to) / size);
            for (int k = lo; k <= hi && n < 3; ++k) {
                const double t = (k * size - from) / step;
                if (t > 1e-6 && t < 1.0 - 1e-6) {
                    at[n] = t;
                    n += 1;
                }
            }
        }
        // In order along the way, from the source (t = 1) back to the listener.
        for (int a = 0; a < n; ++a)
            for (int b = a + 1; b < n; ++b)
                if (at[b] > at[a]) {
                    const double keep = at[a];
                    at[a] = at[b];
                    at[b] = keep;
                }
        px[0] = sx;
        py[0] = sy;
        pz[0] = sz;
        for (int k = 0; k < n; ++k) {
            px[k + 1] = fold(lx + at[k] * dx, w);
            py[k + 1] = fold(ly + at[k] * dy, d);
            pz[k + 1] = fold(lz + at[k] * dz, h);
        }
        px[n + 1] = lx;
        py[n + 1] = ly;
        pz[n + 1] = lz;
        return n + 2;
    }
    double fold(double v, double size) const {
        double m = fmod(v, 2.0 * size);
        if (m < 0.0) m += 2.0 * size;
        return m > size ? 2.0 * size - m : m;
    }

    // The energy an omni at the listener hears in the first t seconds, the
    // direct sound's counted as 1, at 1 kHz.
    double energyBefore(double t) const {
        double e = 1.0;
        for (int i = 0; i < images; ++i)
            if (0.5 * (delay[i] + delay[62 + i]) < t) e += dbToGain(2.0 * shownDb[i]);
        const double t0 = firstOrder + predelay;
        if (t > t0) e += lateOmni * (1.0 - exp(-13.815510557964274 * (t - t0) / rtMid()));
        return e;
    }
    double energyAll() const {
        double e = 1.0 + lateOmni;
        for (int i = 0; i < images; ++i) e += dbToGain(2.0 * shownDb[i]);
        return e;
    }
    // Clarity (C80): the first 80 ms against the rest, dB; definition (D50):
    // the share in the first 50 ms.
    double clarity() const {
        const double before = energyBefore(0.08);
        return 10.0 * log10(before / max(energyAll() - before, 1e-12));
    }
    double definition() const { return energyBefore(0.05) / energyAll(); }

    // The mid-band (500 Hz to 1 kHz) and other readouts.
    double rtMid() const { return 0.5 * (rt[2] + rt[3]); }
    double rtLongest() const {
        double t = 0.0;
        for (int b = 0; b < 7; ++b) t = max(t, rt[b]);
        return t;
    }
};

// --- the parts ------------------------------------------------------------------------

// A line of the past. read(d) is d samples before the sample about to be
// pushed, between samples by a cubic (Hermite); d stays 2 or more.
struct Ring {
    Buffer<float> data;
    int size = 4, at = 0;
    int readAt = 2;  // floor of the last read: the slow wander usually stays in this interval

    void prepare(int n) {
        data.resize(n);
        size = n;
        clear();
    }
    void clear() {
        data.fill(0.0f);
        at = 0;
        readAt = 2;
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
    double read(double d) {
        const double c = clamp(d, 2.0, size - 3.0);
        if (!(c >= readAt && c < readAt + 1.0)) readAt = floorToInt(c);
        const int k = readAt;
        const double f = c - k;
        const double ym = tap(k - 1), y0 = tap(k), y1 = tap(k + 1), y2 = tap(k + 2);
        const double c1 = 0.5 * (y1 - ym);
        const double c2 = ym - 2.5 * y0 + 2.0 * y1 - 0.5 * y2;
        const double c3 = 0.5 * (y2 - ym) + 1.5 * (y0 - y1);
        return ((c3 * f + c2) * f + c1) * f + y0;
    }
};

// An allpass whose length glides, a whole sample at a time (a fraction
// would dull it).
struct Allpass {
    Buffer<float> line;
    int size = 4, at = 0;
    double length = 2.0, target = 2.0;

    void prepare(int n) {
        line.resize(n + 4);
        size = n + 4;
        clear();
    }
    void clear() {
        line.fill(0.0f);
        at = 0;
    }
    double process(double x, double g) {
        const int k = clamp(roundToInt(length), 1, size - 2);
        int i0 = at - k;
        if (i0 < 0) i0 += size;
        const double old = line[i0];
        const double v = x + g * old;
        line[at] = static_cast<float>(v);
        at = at + 1;
        if (at >= size) at = 0;
        return old - g * v;
    }
};

// One delay line of the tail: its length gliding with the room, a slow
// wander, and the loss of each octave over its length (a low shelf, two
// high shelves and a gain).
struct Line {
    Ring ring;
    double length = 100.0, target = 100.0;
    double phaseC = 1.0, phaseS = 0.0, turnC = 1.0, turnS = 0.0;
    double gain = 1.0;
    Biquad low, high, air;
    // The shelves' gains (dB) and the gain, as designed and as heading for:
    // a change glides, the shelves redesigned a little at a time.
    double lowAt = 0.0, highAt = 0.0, airAt = 0.0, gainAt = 1.0;
    double lowTo = 0.0, highTo = 0.0, airTo = 0.0, gainTo = 1.0;

    void clearAll() {
        ring.clear();
        low.clear();
        high.clear();
        air.clear();
    }
};

// The input split into the reflections' three bands, in a line of its own:
// each slot holds the three.
struct Bands {
    Buffer<float> data;
    int size = 4, at = 0;
    OnePole lowSplit, highSplit;

    void prepare(int n) {
        data.resize(3 * n);
        size = n;
        clear();
    }
    void clear() {
        data.fill(0.0f);
        at = 0;
        lowSplit.clear();
        highSplit.clear();
    }
    // Push a sample; the line's newest slot is then 0 samples back.
    void push(double x) {
        const double lo = lowSplit.lowpass(x);
        const double below = highSplit.lowpass(x);
        at = at + 1;
        if (at >= size) at = 0;
        data[3 * at] = static_cast<float>(lo);
        data[3 * at + 1] = static_cast<float>(below - lo);
        data[3 * at + 2] = static_cast<float>(x - below);
    }
    // The three bands summed, the nearest whole sample to d back.
    double whole(double d) const {
        const int k = clamp(roundToInt(d), 0, size - 1);
        int i0 = at - k;
        if (i0 < 0) i0 += size;
        return data[3 * i0] + data[3 * i0 + 1] + data[3 * i0 + 2];
    }
};

// A level's conversion stays the same until its smoothed value moves.
struct DbGain {
    double at = 1000.0, gain = 1.0;

    double get(double db) {
        if (db != at) {
            at = db;
            gain = dbToGain(db);
        }
        return gain;
    }
};

// A sum of signals over a block's frame: the tail's sixteen at once.
void hadamard(Array<double, 16>& v) {
    for (int span = 1; span < 16; span *= 2)
        for (int i = 0; i < 16; i += 2 * span)
            for (int j = i; j < i + span; ++j) {
                const double a = v[j], b = v[j + span];
                v[j] = a + b;
                v[j + span] = a - b;
            }
    for (int i = 0; i < 16; ++i) v[i] *= 0.25;
}

// The sign each line's output has in the left and right sums: two rows of a
// Hadamard matrix, so the two hear the same lines uncorrelated.
double signLeft(int i) { return i % 2 == 0 ? 1.0 : -1.0; }
double signRight(int i) { return (i / 2) % 2 == 0 ? 1.0 : -1.0; }
double signIn(int i) { return (i / 4) % 2 == 0 ? 1.0 : -1.0; }

// The lines' lengths against the mean free path's time, spread over a
// little more than an octave and a half; in an order that alternates long and
// short.
double lineSpread(int i) {
    const int rank = imod(i * 7, 16);
    return 0.62 * pow(2.6, rank / 15.0);
}

bool isPrime(int n) {
    if (n < 2) return false;
    if (imod(n, 2) == 0) return n == 2;
    for (int f = 3; f * f <= n; f += 2)
        if (imod(n, f) == 0) return false;
    return true;
}

// One reflection's current delay and paired clear/blur band weights, with
// their targets beside them. A moving read advances only this path's glide.
struct ReflectedPath {
    double delay = 0.0, targetDelay = 0.0;
    int whole = 0;
    double clearLow = 0.0, blurLow = 0.0, clearMid = 0.0, blurMid = 0.0, clearHigh = 0.0,
           blurHigh = 0.0;
    double clearLowTo = 0.0, blurLowTo = 0.0, clearMidTo = 0.0, blurMidTo = 0.0, clearHighTo = 0.0,
           blurHighTo = 0.0;
    void move(double k) {
        delay += k * (targetDelay - delay);
        clearLow += k * (clearLowTo - clearLow);
        blurLow += k * (blurLowTo - blurLow);
        clearMid += k * (clearMidTo - clearMid);
        blurMid += k * (blurMidTo - blurMid);
        clearHigh += k * (clearHighTo - clearHigh);
        blurHigh += k * (blurHighTo - blurHigh);
    }
    void snap(int size) {
        delay = targetDelay;
        whole = clamp(roundToInt(targetDelay), 0, size - 1);
        clearLow = clearLowTo;
        blurLow = blurLowTo;
        clearMid = clearMidTo;
        blurMid = blurMidTo;
        clearHigh = clearHighTo;
        blurHigh = blurHighTo;
    }
};

struct ReflectedMic {
    Array<ReflectedPath, kImages> paths;
};

// --- the look --------------------------------------------------------------------------

constexpr int kInk = 0x17150F;
constexpr int kGroundTop = 0x24221A;
constexpr int kGroundBottom = 0x1A1913;
constexpr int kGraphTop = 0x1A1914;
constexpr int kGraphBottom = 0x11100C;
constexpr int kGrid = 0x2A281F;
constexpr int kLinen = 0xE6DAC2;  // first-order paths, the listener
constexpr int kKnobLight = 0x4F4A3B;
constexpr int kKnobDark = 0x26241C;
constexpr int kMeterLow = 0x6B7A4A;
constexpr int kMeterHigh = 0xD6CF9E;
constexpr int kLeaf = 0x9DB86A;   // fern: second-order paths, the decay
constexpr int kMoss = 0x5F7448;   // third-order paths, scatter
constexpr int kSun = 0xE8B65C;    // the source, the direct path
constexpr int kBark = 0xA0714A;   // the lows
constexpr int kSky = 0x8DB3C7;    // the highs
constexpr int kFrost = 0xB7CFD6;  // held

// A colour between two, t of the way from a to b.
int blend(int a, int b, double t) {
    const double u = clamp(t, 0.0, 1.0);
    const int ar = idiv(a, 65536), ag = imod(idiv(a, 256), 256), ab = imod(a, 256);
    const int br = idiv(b, 65536), bg = imod(idiv(b, 256), 256), bb = imod(b, 256);
    const int r = roundToInt(ar + (br - ar) * u), g = roundToInt(ag + (bg - ag) * u);
    const int bl = roundToInt(ab + (bb - ab) * u);
    return r * 65536 + g * 256 + bl;
}

struct Ghyll : Effect {
    static constexpr Info info = {
        .name = "Ghyll",
        .id = "com.suppleaudio.ghyll",
        .vendor = "Supple Audio",
        .version = "1.0.0",
        .description =
            "A room built from its walls: size in metres, the surfaces' materials, where the "
            "sound stands and where you listen. Reflections, decay and level follow from the "
            "physics, heard through a pair of microphones or a head.",
        .features = {"reverb"},
        .tail = -1};

    // --- the room -----------------------------------------------------------------------

    Param roomWidth = continuous("Room Width", 1.5, 60, 17, Unit::Meters)
                          .modulated()
                          .curved(Curve::Log)
                          .in("Room")
                          .help("The room from its left wall to its right, metres.");
    Param roomDepth = continuous("Room Depth", 1.5, 100, 25, Unit::Meters)
                          .modulated()
                          .curved(Curve::Log)
                          .in("Room")
                          .help("The room from its front wall to its back, metres.");
    Param roomHeight = continuous("Room Height", 2, 40, 11, Unit::Meters)
                           .modulated()
                           .curved(Curve::Log)
                           .in("Room")
                           .help("The room from floor to ceiling, metres.");
    Param walls = choice("Walls",
                         {"Stone", "Concrete", "Brick", "Plaster", "Wood", "Glass", "Carpet",
                          "Curtains", "Audience", "Acoustic", "Open"},
                         3)
                      .in("Room")
                      .help("What the four walls are made of: open walls let the sound go.");
    Param floor = choice("Floor",
                         {"Stone", "Concrete", "Brick", "Plaster", "Wood", "Glass", "Carpet",
                          "Curtains", "Audience", "Acoustic", "Open"},
                         8)
                      .in("Room")
                      .help("What the floor is: audience is a seated audience, or seats.");
    Param ceiling = choice("Ceiling",
                           {"Stone", "Concrete", "Brick", "Plaster", "Wood", "Glass", "Carpet",
                            "Curtains", "Audience", "Acoustic", "Open"},
                           4)
                        .in("Room")
                        .help("What the ceiling is: open is the sky, a courtyard's.");
    Param decay = percent("Decay", 100, 25, 400)
                      .modulated()
                      .curved(Curve::Log)
                      .in("Room")
                      .help(
                          "The reverberation time against the room as built: 200% is twice as "
                          "long (the surfaces and the air absorb half as much).");
    Param scatter = percent("Scatter", 40)
                        .modulated()
                        .in("Room")
                        .help(
                            "How rough the surfaces are: 0 is smooth walls and clear, discrete "
                            "reflections; 100 spreads each into a blur.");

    // --- the places ---------------------------------------------------------------------

    Param sourceX = percent("Source X", 44)
                        .modulated()
                        .in("Places")
                        .help("Where the sound stands across the room, from the left wall.");
    Param sourceY = percent("Source Y", 22)
                        .modulated()
                        .in("Places")
                        .help("Where the sound stands, back from the front wall.");
    Param listenerX = percent("Listener X", 54)
                          .modulated()
                          .in("Places")
                          .help("Where you listen across the room, from the left wall.");
    Param listenerY = percent("Listener Y", 44)
                          .modulated()
                          .in("Places")
                          .help("Where you listen, back from the front wall.");
    Param mic = choice("Mic", {"ORTF", "Spaced", "Head", "Mono"}, 0)
                    .in("Places")
                    .help(
                        "How the room is heard: an ORTF pair (natural stereo), spaced omnis "
                        "(wide, airy), a head (as you would hear it there, for headphones) or "
                        "one omni (mono).");

    // --- the output ---------------------------------------------------------------------

    Param predelay = milliseconds("Predelay", 0, 250, 0)
                         .modulated()
                         .in("Output")
                         .help("A gap added before the room answers, to keep a voice clear of it.");
    Param early = decibels("Early", -30, 12, 0)
                      .modulated()
                      .offAtMin()
                      .smoothed(20)
                      .in("Output")
                      .help("The reflections against the room's own level; -30 is off.");
    Param late = decibels("Late", -30, 12, 0)
                     .modulated()
                     .offAtMin()
                     .smoothed(20)
                     .in("Output")
                     .help("The tail against the room's own level; -30 is off.");
    Param lowCut = frequency("Low Cut", 20, 1000, 20)
                       .modulated()
                       .in("Output")
                       .help("Thins the room (the dry sound is untouched); 20 Hz is off.");
    Param highCut = frequency("High Cut", 1000, 20000, 20000)
                        .modulated()
                        .in("Output")
                        .help("Darkens the room (the dry sound is untouched); 20 kHz is off.");
    Param width = percent("Width", 100, 0, 200)
                      .modulated()
                      .smoothed(20)
                      .in("Output")
                      .help("The room's stereo width: 0 is mono, 200 twice as wide.");
    Param freeze = toggle("Freeze", false)
                       .in("Output")
                       .help("Hold the tail as it is, forever, and let no new sound in.");
    Param mix = percent("Mix", 25).modulated().smoothed(20).in("Output").help(
        "Dry and room: the dry stays full up to 50% and the room reaches full there, the balance "
        "heard from the listener's place; 100% is the room alone.");
    Param output =
        decibels("Output", -24, 24, 0).modulated().smoothed(20).in("Output").help("The level out.");

    Param inputLevel = decibels("Input Level", -96, 24, -96)
                           .meter()
                           .help("The input's peak, dBFS, falling 20 dB a second.");
    Param outputLevel = decibels("Output Level", -96, 24, -96)
                            .meter()
                            .help("The output's peak, dBFS, falling 20 dB a second.");
    Param roomLevel = decibels("Room Level", -96, 24, -96)
                          .meter()
                          .help("The room's peak before the mix, dBFS, falling 20 dB a second.");
    Param rt60 = milliseconds("RT60", 0, 600000, 0)
                     .meter()
                     .help("The time the room takes to fall 60 dB at 500 Hz to 1 kHz.");
    Param rt60Low = milliseconds("RT60 Low", 0, 600000, 0)
                        .meter()
                        .help("The time the room takes to fall 60 dB at 125 Hz.");
    Param rt60High = milliseconds("RT60 High", 0, 600000, 0)
                         .meter()
                         .help("The time the room takes to fall 60 dB at 4 kHz.");
    Param distance = continuous("Distance", 0, 200, 0, Unit::Meters)
                         .meter()
                         .help("From the source to the listener.");
    Param criticalDistance =
        continuous("Critical Distance", 0, 200, 0, Unit::Meters)
            .meter()
            .help(
                "Where the room grows as loud as the direct sound: a listener farther away hears "
                "mostly room.");
    Param clarity = decibels("Clarity", -30, 30, 0)
                        .meter()
                        .help(
                            "C80: the first 80 ms against the rest, dB (with the dry sound); music "
                            "sounds clear above 0, concert halls run -3 to +3.");
    Param definition = percent("Definition", 0)
                           .meter()
                           .help(
                               "D50: the share of the sound in its first 50 ms (with the dry "
                               "sound); speech is clear above 50%.");
    Param firstReflection =
        milliseconds("First Reflection", 0, 2000, 0)
            .meter()
            .help("The gap from the direct sound to the first reflection, predelay included.");

    void factoryPatches(Patches& p) {
        p.add("Recital Hall");
        p.add("Concert Hall")
            .set(roomWidth, 26)
            .set(roomDepth, 40)
            .set(roomHeight, 16)
            .set(walls, "plaster")
            .set(floor, "audience")
            .set(ceiling, "wood")
            .set(sourceX, 46)
            .set(sourceY, 14)
            .set(listenerX, 54)
            .set(listenerY, 42)
            .set(mix, 30);
        p.add("Back of the Hall")
            .set(roomWidth, 26)
            .set(roomDepth, 40)
            .set(roomHeight, 16)
            .set(walls, "plaster")
            .set(floor, "audience")
            .set(ceiling, "wood")
            .set(sourceX, 46)
            .set(sourceY, 14)
            .set(listenerX, 40)
            .set(listenerY, 88)
            .set(mix, 15);
        p.add("Hall on Headphones")
            .set(roomWidth, 26)
            .set(roomDepth, 40)
            .set(roomHeight, 16)
            .set(walls, "plaster")
            .set(floor, "audience")
            .set(ceiling, "wood")
            .set(sourceX, 46)
            .set(sourceY, 14)
            .set(listenerX, 54)
            .set(listenerY, 42)
            .set(mic, "head")
            .set(mix, 30);
        p.add("Small Church")
            .set(roomWidth, 12)
            .set(roomDepth, 30)
            .set(roomHeight, 12)
            .set(walls, "stone")
            .set(floor, "wood")
            .set(ceiling, "plaster")
            .set(sourceX, 50)
            .set(sourceY, 14)
            .set(listenerX, 42)
            .set(listenerY, 55)
            .set(decay, 55)
            .set(mix, 15);
        p.add("Cathedral")
            .set(roomWidth, 24)
            .set(roomDepth, 70)
            .set(roomHeight, 26)
            .set(walls, "stone")
            .set(floor, "wood")
            .set(ceiling, "wood")
            .set(sourceX, 50)
            .set(sourceY, 12)
            .set(listenerX, 46)
            .set(listenerY, 48)
            .set(scatter, 55)
            .set(decay, 70)
            .set(mix, 15);
        p.add("Live Room")
            .set(roomWidth, 8)
            .set(roomDepth, 11)
            .set(roomHeight, 4.5)
            .set(walls, "wood")
            .set(floor, "wood")
            .set(ceiling, "acoustic")
            .set(sourceX, 42)
            .set(sourceY, 35)
            .set(listenerX, 58)
            .set(listenerY, 62)
            .set(scatter, 50)
            .set(mix, 25);
        p.add("Drum Room")
            .set(roomWidth, 7)
            .set(roomDepth, 9)
            .set(roomHeight, 4)
            .set(walls, "brick")
            .set(floor, "wood")
            .set(ceiling, "plaster")
            .set(sourceX, 50)
            .set(sourceY, 40)
            .set(listenerX, 50)
            .set(listenerY, 72)
            .set(scatter, 35)
            .set(decay, 45)
            .set(mix, 20);
        p.add("Vocal Booth")
            .set(roomWidth, 2.2)
            .set(roomDepth, 2)
            .set(roomHeight, 2.4)
            .set(walls, "acoustic")
            .set(floor, "carpet")
            .set(ceiling, "acoustic")
            .set(sourceX, 50)
            .set(sourceY, 45)
            .set(listenerX, 50)
            .set(listenerY, 70)
            .set(mix, 50);
        p.add("Living Room")
            .set(roomWidth, 5)
            .set(roomDepth, 6)
            .set(roomHeight, 2.6)
            .set(walls, "plaster")
            .set(floor, "carpet")
            .set(ceiling, "plaster")
            .set(sourceX, 40)
            .set(sourceY, 35)
            .set(listenerX, 60)
            .set(listenerY, 70)
            .set(scatter, 60)
            .set(decay, 50)
            .set(mix, 25);
        p.add("Tiled Bathroom")
            .set(roomWidth, 2.5)
            .set(roomDepth, 3)
            .set(roomHeight, 2.5)
            .set(walls, "stone")
            .set(floor, "stone")
            .set(ceiling, "plaster")
            .set(sourceX, 35)
            .set(sourceY, 40)
            .set(listenerX, 60)
            .set(listenerY, 70)
            .set(scatter, 20)
            .set(mix, 15);
        p.add("Echo Chamber")
            .set(roomWidth, 5)
            .set(roomDepth, 7)
            .set(roomHeight, 4)
            .set(walls, "concrete")
            .set(floor, "concrete")
            .set(ceiling, "concrete")
            .set(sourceX, 20)
            .set(sourceY, 15)
            .set(listenerX, 80)
            .set(listenerY, 85)
            .set(scatter, 70)
            .set(mix, 8);
        p.add("Stairwell")
            .set(roomWidth, 3)
            .set(roomDepth, 4)
            .set(roomHeight, 20)
            .set(walls, "concrete")
            .set(floor, "concrete")
            .set(ceiling, "concrete")
            .set(sourceX, 40)
            .set(sourceY, 30)
            .set(listenerX, 60)
            .set(listenerY, 70)
            .set(scatter, 15)
            .set(mix, 25);
        p.add("Courtyard")
            .set(roomWidth, 20)
            .set(roomDepth, 20)
            .set(roomHeight, 15)
            .set(walls, "brick")
            .set(floor, "stone")
            .set(ceiling, "open")
            .set(sourceX, 40)
            .set(sourceY, 35)
            .set(listenerX, 60)
            .set(listenerY, 65)
            .set(scatter, 45)
            .set(mix, 30);
        p.add("Parking Garage")
            .set(roomWidth, 40)
            .set(roomDepth, 60)
            .set(roomHeight, 2.8)
            .set(walls, "concrete")
            .set(floor, "concrete")
            .set(ceiling, "concrete")
            .set(sourceX, 40)
            .set(sourceY, 40)
            .set(listenerX, 55)
            .set(listenerY, 55)
            .set(scatter, 20)
            .set(mix, 20);
        p.add("Warehouse")
            .set(roomWidth, 30)
            .set(roomDepth, 50)
            .set(roomHeight, 10)
            .set(walls, "brick")
            .set(floor, "concrete")
            .set(ceiling, "wood")
            .set(sourceX, 40)
            .set(sourceY, 30)
            .set(listenerX, 55)
            .set(listenerY, 60)
            .set(decay, 60)
            .set(mix, 20);
    }

    // --- audio --------------------------------------------------------------------------

    double rate = 48000.0;
    Room room;
    Bands bands;
    Array<Line, 16> lines;
    Array<Allpass, 4> into;   // the tail's input diffusion
    Array<Allpass, 6> blurs;  // scatter's, three per microphone
    OnePole side;             // the tail's lows toward the centre
    Array<Biquad, 2> lowCutF, highCutF;

    // The modes, a resonator each, and the crossover between them and the tail.
    Array<double, 48> modeY1, modeY2, modeA1, modeA2, modeB, modeR, modeCos, modeG0, modeG1;
    int modeCount = 0;
    Biquad modeIn, tailIn;
    Array<Biquad, 2> modeOut;
    double atCrossover = -1.0;

    // What the reflections are heading for, and where they are.
    Array<ReflectedMic, 2> reflected;
    int moving = 0;  // samples the reflections still glide
    double glide = 0.001, lineGlide = 0.001, lateAt = 0.0, lateTo = 0.0;
    double injectAt = 1.0, injectTo = 1.0;  // samples: where the tail's input is read
    bool unplayed = true;

    // Onsets, for the editor's playhead.
    double fastEnv = 0.0, slowEnv = 0.0, fastUp = 0.1, fastDown = 0.01, slowUp = 0.01,
           slowDown = 0.001;
    int sinceHit = 1000000000, hitHold = 4800;

    Shared<double> inLeft, inRight, outLeft, outRight, roomLeft, roomRight, sinceHitShown;
    Shared<bool> frozenShown;

    double frozenW = 0.0, freezeStep = 0.001, lowW = 0.0, highW = 0.0, cutStep = 0.001;
    double modDepth = 0.0, monoW = 0.0;
    Biquad fitLow, fitHigh, fitAir;  // the fit's own, never heard
    int lossMoving = 0,
        lossCount = 0;  // the lines' designs: steps still to go, samples to the next
    double clarityNow = 0.0, definitionNow = 0.0;
    double peakIn0 = 0.0, peakIn1 = 0.0, peakOut0 = 0.0, peakOut1 = 0.0, peakRoom = 0.0,
           peakRoom0 = 0.0, peakRoom1 = 0.0, fall = 0.999;
    double atLow = -1.0, atHigh = -1.0;
    DbGain earlyGain, lateGain, outputGain;

    void prepare() {
        rate = sampleRate();
        bands.prepare(ceilToInt((kReach + kPredelayLongest + 0.01) * rate) + 4);
        bands.lowSplit.setCutoff(500.0);
        bands.highSplit.setCutoff(min(4000.0, 0.4 * rate));
        for (int i = 0; i < kLines; ++i) {
            lines[i].ring.prepare(ceilToInt(kLineLongest * rate) + 8);
            lines[i].clearAll();
            // A slow wander each, 0.13 to 0.9 Hz.
            const double hz = 0.13 + 0.77 * imod(i * 5, 16) / 15.0;
            lines[i].turnC = cos(twoPi * hz / rate);
            lines[i].turnS = sin(twoPi * hz / rate);
            lines[i].phaseC = cos(i * 2.399963229728653);
            lines[i].phaseS = sin(i * 2.399963229728653);
        }
        for (int k = 0; k < 4; ++k) {
            into[k].prepare(ceilToInt(0.06 * rate));
            into[k].clear();
        }
        for (int k = 0; k < 6; ++k) {
            blurs[k].prepare(ceilToInt(0.03 * rate));
            blurs[k].clear();
        }
        side.clear();
        for (int c = 0; c < 2; ++c) {
            lowCutF[c].clear();
            highCutF[c].clear();
            modeOut[c].clear();
        }
        modeIn.clear();
        tailIn.clear();
        modeY1.fill(0.0);
        modeY2.fill(0.0);
        atCrossover = -1.0;
        glide = 1.0 - exp(-1.0 / (0.05 * rate));
        lineGlide = 1.0 - exp(-1.0 / (0.15 * rate));
        freezeStep = 1.0 / (0.05 * rate);
        cutStep = 1.0 / (0.01 * rate);
        fall = dbToGain(-20.0 / rate);
        fastUp = 1.0 - exp(-1.0 / (0.0005 * rate));
        fastDown = 1.0 - exp(-1.0 / (0.03 * rate));
        slowUp = 1.0 - exp(-1.0 / (0.03 * rate));
        slowDown = 1.0 - exp(-1.0 / (0.3 * rate));
        hitHold = roundToInt(0.15 * rate);
        fastEnv = 0.0;
        slowEnv = 0.0;
        sinceHit = 1000000000;
        frozenW = 0.0;
        peakIn0 = 0.0;
        peakIn1 = 0.0;
        peakOut0 = 0.0;
        peakOut1 = 0.0;
        peakRoom = 0.0;
        peakRoom0 = 0.0;
        peakRoom1 = 0.0;
        atLow = -1.0;
        atHigh = -1.0;
        unplayed = true;
        rebuild(true);
        snap();
    }

    void update() {
        const bool lossesChanged = roomWidth.changed() || roomDepth.changed() ||
                                   roomHeight.changed() || walls.changed() || floor.changed() ||
                                   ceiling.changed() || decay.changed();
        if (lossesChanged || scatter.changed() || sourceX.changed() || sourceY.changed() ||
            listenerX.changed() || listenerY.changed() || mic.changed() || predelay.changed()) {
            rebuild(lossesChanged);
        } else {
            // Output controls leave the room and the fitted losses unchanged.
            // Keep the loss glide's schedule, including during an existing glide.
            lossMoving = 24;
            reportTail();
        }
        if (unplayed) snap();
    }

    // The room as set, and everything that follows from it.
    void rebuild(bool lossesChanged) {
        room.w = roomWidth;
        room.d = roomDepth;
        room.h = roomHeight;
        room.walls = walls.index();
        room.floor = floor.index();
        room.ceiling = ceiling.index();
        room.mic = mic.index();
        room.scale = decay;
        room.scatter = scatter;
        room.predelay = predelay * 0.001;
        room.sxp = sourceX;
        room.syp = sourceY;
        room.lxp = listenerX;
        room.lyp = listenerY;
        room.rate = rate;
        room.compute();
        // (Whole samples: a glide starts and ends on one, where the settled
        // reflections read.)
        for (int m = 0; m < 2; ++m)
            for (int i = 0; i < 62; ++i) {
                ReflectedPath& path = reflected[m].paths[i];
                const int t = 62 * m + i, q = 3 * t;
                const double d = clamp(roundToInt(room.delay[t] * rate), 0, bands.size - 1);
                if (abs(d - path.targetDelay) > 1e-6) moving = roundToInt(0.4 * rate);
                path.targetDelay = d;
                if (abs(room.spec[q] - path.clearLowTo) > 1e-9 ||
                    abs(room.blur[q] - path.blurLowTo) > 1e-9)
                    moving = roundToInt(0.4 * rate);
                path.clearLowTo = room.spec[q];
                path.blurLowTo = room.blur[q];
                if (abs(room.spec[q + 1] - path.clearMidTo) > 1e-9 ||
                    abs(room.blur[q + 1] - path.blurMidTo) > 1e-9)
                    moving = roundToInt(0.4 * rate);
                path.clearMidTo = room.spec[q + 1];
                path.blurMidTo = room.blur[q + 1];
                if (abs(room.spec[q + 2] - path.clearHighTo) > 1e-9 ||
                    abs(room.blur[q + 2] - path.blurHighTo) > 1e-9)
                    moving = roundToInt(0.4 * rate);
                path.clearHighTo = room.spec[q + 2];
                path.blurHighTo = room.blur[q + 2];
            }
        // The modes.
        for (int n = 0; n < 48; ++n) {
            if (n >= room.modes) {
                modeG0[n] = 0.0;
                modeG1[n] = 0.0;
                continue;
            }
            const double wn = twoPi * room.modeHz[n] / rate;
            const double r = exp(-room.modeDecay[n] / rate);
            modeR[n] = r;
            modeCos[n] = cos(wn);
            modeA1[n] = 2.0 * r * modeCos[n];
            modeA2[n] = r * r;
            modeB[n] = sin(wn);
            modeG0[n] = room.modeGain[n];
            modeG1[n] = room.modeGain[48 + n];
        }
        modeCount = room.modes;
        const double fx = room.crossover;
        if (abs(fx - atCrossover) > 1e-3 * fx) {
            atCrossover = fx;
            modeIn.lowpass(fx, 0.7071067811865476);
            tailIn.highpass(fx, 0.7071067811865476);
            for (int c = 0; c < 2; ++c) modeOut[c].lowpass(1.25 * fx, 0.7071067811865476);
        }
        // The tail: lines as long as the mean free path (8 ms at the least),
        // each a prime number of samples.
        // (A small room that rings long needs more of them than its size
        // gives, or its tail rings: at least 20 ms per second of decay.)
        const double path =
            clamp(max(room.freePath / kSound, kDensity * room.rtMid()), 0.008, 0.11);
        double mean = 0.0;
        for (int i = 0; i < kLines; ++i) {
            int n = max(16, roundToInt(path * lineSpread(i) * rate));
            while (!isPrime(n)) n += 1;
            lines[i].target = n;
            mean += n;
        }
        mean = mean / kLines;
        if (lossesChanged)
            for (int i = 0; i < kLines; ++i) fitLine(lines[i]);
        lossMoving = 24;
        // The input's diffusion, about a free path long.
        for (int k = 0; k < 4; ++k) {
            const double share = k == 0 ? 0.11 : (k == 1 ? 0.17 : (k == 2 ? 0.27 : 0.41));
            into[k].target = clamp(path * share * rate, 1.0, 0.055 * rate);
        }
        for (int k = 0; k < 6; ++k) {
            const double share = imod(k, 3) == 0 ? 0.05 : (imod(k, 3) == 1 ? 0.083 : 0.131);
            blurs[k].target = clamp(path * share * (k < 3 ? 1.0 : 1.13) * rate, 1.0, 0.028 * rate);
        }
        // The tail's level: the energy it must carry, over what the lines give
        // an impulse (a sixteenth of what circulates, which lasts as long as the
        // mid-band decay allows).
        const double perPass = exp(-13.815510557964274 * (mean / rate) / room.rtMid());
        const double lineEnergy = perPass / (16.0 * (1.0 - perPass));
        lateTo = room.lateGain / sqrt(lineEnergy);
        // The tail takes over as the first-order reflections arrive: its
        // shortest line hears the input that long before.
        double shortest = path * lineSpread(0) * rate;
        for (int i = 1; i < kLines; ++i) {
            const double n = path * lineSpread(i) * rate;
            if (n < shortest) shortest = n;
        }
        injectTo = max(
            2.0, max(room.first * rate, room.firstOrder * rate - shortest) + room.predelay * rate);
        modDepth = 0.00025 * rate * min(1.0, path / 0.02);
        side.setCutoff(room.sideHz);
        clarityNow = clamp(room.clarity(), -30.0, 30.0);
        definitionNow = room.definition();
        reportTail();
    }

    // A line's loss over its length: each octave's decay (-60 dB in its
    // RT60), fitted with a low shelf, two high shelves (the surfaces, the air)
    // and a gain; where the line's design is to go.
    void fitLine(Line& l) {
        const double seconds = l.target / rate;
        Array<double, 7> want;
        for (int b = 0; b < 7; ++b) want[b] = -60.0 * seconds / room.rt[b];
        const double mid = 0.5 * (want[2] + want[3]);
        double lowDb = want[0] - mid, highDb = want[5] - mid, airDb = want[6] - want[5];
        double base = mid;
        // Three rounds: each shelf's gain corrected by what the others do at its octave.
        for (int round = 0; round < 3; ++round) {
            fitLow.lowShelf(200.0, 0.6, lowDb);
            fitHigh.highShelf(2500.0, 0.6, highDb);
            fitAir.highShelf(airHz(), 0.6, airDb);
            base = mid - (fitLow.responseDb(707.0) + fitHigh.responseDb(707.0) +
                          fitAir.responseDb(707.0));
            const double got0 = base + fitLow.responseDb(125.0) + fitHigh.responseDb(125.0) +
                                fitAir.responseDb(125.0);
            const double got5 = base + fitLow.responseDb(4000.0) + fitHigh.responseDb(4000.0) +
                                fitAir.responseDb(4000.0);
            const double got6 = base + fitLow.responseDb(8000.0) + fitHigh.responseDb(8000.0) +
                                fitAir.responseDb(8000.0);
            lowDb += want[0] - got0;
            highDb += want[5] - got5;
            airDb += want[6] - got6 - (want[5] - got5);
        }
        fitLow.lowShelf(200.0, 0.6, lowDb);
        fitHigh.highShelf(2500.0, 0.6, highDb);
        fitAir.highShelf(airHz(), 0.6, airDb);
        base =
            mid - (fitLow.responseDb(707.0) + fitHigh.responseDb(707.0) + fitAir.responseDb(707.0));
        l.lowTo = lowDb;
        l.highTo = highDb;
        l.airTo = airDb;
        l.gainTo = dbToGain(base);
    }
    double airHz() const { return min(7000.0, 0.4 * rate); }

    // A line's filters as its glide has them.
    void designLine(Line& l) {
        l.low.lowShelf(200.0, 0.6, l.lowAt);
        l.high.highShelf(2500.0, 0.6, l.highAt);
        l.air.highShelf(airHz(), 0.6, l.airAt);
        l.gain = l.gainAt;
    }

    // The lines' designs a step nearer where they are going (every 32 samples).
    void stepLines() {
        lossMoving -= 1;
        const double k = lossMoving <= 0 ? 1.0 : 0.25;
        for (int j = 0; j < kLines; ++j) {
            Line& l = lines[j];
            const double low = l.lowAt, high = l.highAt, air = l.airAt, gain = l.gainAt;
            l.lowAt += k * (l.lowTo - l.lowAt);
            l.highAt += k * (l.highTo - l.highAt);
            l.airAt += k * (l.airTo - l.airAt);
            l.gainAt += k * (l.gainTo - l.gainAt);
            if (l.lowAt != low || l.highAt != high || l.airAt != air || l.gainAt != gain)
                designLine(l);
        }
    }

    // The reflections where they were heading, in whole samples.
    void settle() {
        for (auto& receiver : reflected)
            for (auto& path : receiver.paths) path.snap(bands.size);
    }

    void snap() {
        settle();
        moving = 0;
        for (int i = 0; i < kLines; ++i) {
            Line& l = lines[i];
            l.length = l.target;
            l.lowAt = l.lowTo;
            l.highAt = l.highTo;
            l.airAt = l.airTo;
            l.gainAt = l.gainTo;
            designLine(l);
        }
        lossMoving = 0;
        for (int k = 0; k < 4; ++k) into[k].length = into[k].target;
        for (int k = 0; k < 6; ++k) blurs[k].length = blurs[k].target;
        lateAt = lateTo;
        injectAt = injectTo;
        frozenW = freeze.on() ? 1.0 : 0.0;
        monoW = mic.index() == 3 ? 1.0 : 0.0;
        lowW = lowCut > 20.5 ? 1.0 : 0.0;
        highW = highCut < 19500.0 ? 1.0 : 0.0;
    }

    // The tail the host is told: the slowest octave to -80 dB, after the
    // reflections and the predelay.
    void reportTail() {
        if (freeze.on()) {
            setTail(-1);
            return;
        }
        const double seconds = room.predelay + room.first + 0.2 + room.rtLongest() * 80.0 / 60.0;
        setTail(min(roundToInt(seconds * rate), 2000000000));
    }

    void retune(double lc, double hc) {
        if (abs(lc - atLow) > 1e-3 * lc) {
            atLow = lc;
            for (int c = 0; c < 2; ++c) lowCutF[c].highpass(lc, 0.7071067811865476);
        }
        if (abs(hc - atHigh) > 1e-3 * hc) {
            atHigh = hc;
            for (int c = 0; c < 2; ++c)
                highCutF[c].lowpass(min(hc, 0.45 * rate), 0.7071067811865476);
        }
    }

    void process(Block& io) {
        const int chans = io.channels;
        const bool frozenNow = freeze.on();
        const bool toMono = room.mic == 3;
        Array<double, 16> v;
        for (int i = 0; i < io.frames; ++i) {
            unplayed = false;
            frozenW = frozenNow ? min(1.0, frozenW + freezeStep) : max(0.0, frozenW - freezeStep);
            const double lc = lowCut.value(), hc = highCut.value();
            lowW = lc > 20.5 ? min(1.0, lowW + cutStep) : max(0.0, lowW - cutStep);
            highW = hc < 19500.0 ? min(1.0, highW + cutStep) : max(0.0, highW - cutStep);
            retune(lc, hc);

            double l = io[0][i];
            double r = chans > 1 ? io[1][i] : l;
            if (!isFinite(l)) l = 0.0;
            if (!isFinite(r)) r = 0.0;
            const double open = 1.0 - frozenW;
            bands.push(0.5 * (l + r) * open);

            // Onsets, for the editor's playhead.
            const double heard = max(abs(l), abs(r)) * open;
            fastEnv += (heard > fastEnv ? fastUp : fastDown) * (heard - fastEnv);
            slowEnv += (heard > slowEnv ? slowUp : slowDown) * (heard - slowEnv);
            sinceHit = min(sinceHit + 1, 1000000000);
            if (fastEnv > 2.0 * slowEnv && fastEnv > 0.006 && sinceHit > hitHold) sinceHit = 0;

            // One omni settled: one microphone's reflections serve both.
            const int mics = toMono && moving == 0 ? 1 : 2;
            monoW = toMono ? min(1.0, monoW + freezeStep) : max(0.0, monoW - freezeStep);

            Array<double, 2> clearSum, blurSum;
            if (moving == 0) {
                // Settled: each reflection at its whole sample, undulled.
                for (int m = 0; m < mics; ++m) {
                    double cs = 0.0, bs = 0.0;
                    const ReflectedMic& receiver = reflected[m];
                    for (int k = 0; k < kImages; ++k) {
                        const ReflectedPath& path = receiver.paths[k];
                        int i0 = bands.at - path.whole;
                        if (i0 < 0) i0 += bands.size;
                        const double x0 = bands.data[3 * i0], x1 = bands.data[3 * i0 + 1];
                        const double x2 = bands.data[3 * i0 + 2];
                        cs += path.clearLow * x0 + path.clearMid * x1 + path.clearHigh * x2;
                        bs += path.blurLow * x0 + path.blurMid * x1 + path.blurHigh * x2;
                    }
                    clearSum[m] = cs;
                    blurSum[m] = bs;
                }
            }
            for (int m = 0; m < mics && moving > 0; ++m) {
                double cs = 0.0, bs = 0.0;
                ReflectedMic& receiver = reflected[m];
                for (int k = 0; k < kImages; ++k) {
                    ReflectedPath& path = receiver.paths[k];
                    path.move(glide);
                    const double dd = clamp(path.delay, 0.0, bands.size - 2.0);
                    const int n = floorToInt(dd);
                    const double f = dd - n;
                    int i0 = bands.at - n, i1 = bands.at - n - 1;
                    if (i0 < 0) i0 += bands.size;
                    if (i1 < 0) i1 += bands.size;
                    const double x0 =
                        bands.data[3 * i0] + f * (bands.data[3 * i1] - bands.data[3 * i0]);
                    const double x1 = bands.data[3 * i0 + 1] +
                                      f * (bands.data[3 * i1 + 1] - bands.data[3 * i0 + 1]);
                    const double x2 = bands.data[3 * i0 + 2] +
                                      f * (bands.data[3 * i1 + 2] - bands.data[3 * i0 + 2]);
                    cs += path.clearLow * x0 + path.clearMid * x1 + path.clearHigh * x2;
                    bs += path.blurLow * x0 + path.blurMid * x1 + path.blurHigh * x2;
                }
                clearSum[m] = cs;
                blurSum[m] = bs;
            }
            Array<double, 2> er;
            for (int m = 0; m < mics; ++m) {
                double b = blurSum[m];
                for (int k = 0; k < 3; ++k) {
                    Allpass& a = blurs[m * 3 + k];
                    a.length += lineGlide * (a.target - a.length);
                    b = a.process(b, 0.6);
                }
                er[m] = clearSum[m] + b;
            }
            if (mics == 1) er[1] = er[0];
            if (moving > 0) {
                moving -= 1;
                if (moving == 0) settle();
            }

            if (lossMoving > 0) {
                lossCount += 1;
                if (lossCount >= 32) {
                    lossCount = 0;
                    stepLines();
                }
            }

            // The tail: the input after the first reflection, diffused, into
            // the lines.
            injectAt += lineGlide * (injectTo - injectAt);
            const double heardNow = bands.whole(injectAt);
            const double xm = modeIn.process(heardNow) * open;
            double x = tailIn.process(heardNow);
            for (int k = 0; k < 4; ++k) {
                into[k].length += lineGlide * (into[k].target - into[k].length);
                x = into[k].process(x, 0.62);
            }
            x *= 0.25 * open;
            const double depth = modDepth * open;
            double tl = 0.0, tr = 0.0;
            for (int j = 0; j < kLines; ++j) {
                Line& ln = lines[j];
                ln.length += lineGlide * (ln.target - ln.length);
                const double c = ln.phaseC, s = ln.phaseS;
                ln.phaseC = c * ln.turnC - s * ln.turnS;
                ln.phaseS = s * ln.turnC + c * ln.turnS;
                const double y = ln.ring.read(ln.length + depth * (1.0 + ln.phaseS));
                const double y1 = ln.low.process(y);
                const double y2 = ln.high.process(y1);
                const double y3 = ln.air.process(y2);
                const double z = ln.gain * y3;
                const double kept = z + frozenW * (y - z);
                v[j] = kept;
                tl += signLeft(j) * kept;
                tr += signRight(j) * kept;
            }
            hadamard(v);
            for (int j = 0; j < kLines; ++j) lines[j].ring.push(v[j] + signIn(j) * x);
            lateAt += glide * (lateTo - lateAt);
            const double lg = lateAt * 0.25;
            tl *= lg;
            tr *= lg;
            {
                // The lows of the tail toward the centre, as the capsules hear
                // them; one omni hears it all in the middle.
                const double mid = 0.5 * (tl + tr), apart = 0.5 * (tl - tr);
                const double s = apart - side.lowpass(apart);
                const double m = mid * (1.0 + monoW * 0.4142135623730951);
                tl = m + s * (1.0 - monoW);
                tr = m - s * (1.0 - monoW);
            }

            // The modes, held as the tail is when frozen.
            double m0 = 0.0, m1 = 0.0;
            for (int n = 0; n < modeCount; ++n) {
                double a1 = modeA1[n], a2 = modeA2[n];
                if (frozenW > 0.0) {
                    const double r = modeR[n] + frozenW * (1.0 - modeR[n]);
                    a1 = 2.0 * r * modeCos[n];
                    a2 = r * r;
                }
                const double y = modeB[n] * xm + a1 * modeY1[n] - a2 * modeY2[n];
                modeY2[n] = modeY1[n];
                modeY1[n] = y;
                m0 += modeG0[n] * y;
                m1 += modeG1[n] * y;
            }
            tl += modeOut[0].process(m0);
            tr += modeOut[1].process(m1);

            // The room: reflections and tail, cuts, width, mix.
            const double eg = levelGain(early.at(i), earlyGain);
            const double lgain = levelGain(late.at(i), lateGain);
            double wl = er[0] * eg + tl * lgain;
            double wr = er[1] * eg + tr * lgain;
            if (lowW > 0.0) {
                wl += lowW * (lowCutF[0].process(wl) - wl);
                wr += lowW * (lowCutF[1].process(wr) - wr);
            }
            if (highW > 0.0) {
                wl += highW * (highCutF[0].process(wl) - wl);
                wr += highW * (highCutF[1].process(wr) - wr);
            }
            const double wd = width.at(i);
            if (wd != 1.0) {
                const double mid = 0.5 * (wl + wr), side = 0.5 * (wl - wr) * wd;
                wl = mid + side;
                wr = mid - side;
            }
            const double mx = mix.at(i);
            const double dry = min(1.0, 2.0 * (1.0 - mx)), wet = min(1.0, 2.0 * mx);
            const double gainOut = outputGain.get(output.at(i));
            const double ol = (l * dry + wl * wet) * gainOut;
            const double orr = (r * dry + wr * wet) * gainOut;
            io[0][i] = ol;
            if (chans > 1) io[1][i] = orr;

            peakIn0 = max(abs(l), peakIn0 * fall);
            peakIn1 = max(abs(r), peakIn1 * fall);
            peakOut0 = max(abs(ol), peakOut0 * fall);
            peakOut1 = max(abs(orr), peakOut1 * fall);
            peakRoom = max(max(abs(wl), abs(wr)), peakRoom * fall);
            peakRoom0 = max(abs(wl), peakRoom0 * fall);
            peakRoom1 = max(abs(wr), peakRoom1 * fall);
        }
        publish();
    }

    // A level that reaches nothing at -30 dB: the last 6 dB fade out.
    double levelGain(double db, DbGain& cache) const {
        if (db <= -29.99) return 0.0;
        const double g = cache.get(db);
        return db < -24.0 ? g * (db + 30.0) / 6.0 : g;
    }

    void publish() {
        inputLevel.set(gainToDb(max(peakIn0, peakIn1)));
        outputLevel.set(gainToDb(max(peakOut0, peakOut1)));
        roomLevel.set(gainToDb(peakRoom));
        rt60.set(room.rtMid() * 1000.0);
        rt60Low.set(room.rt[0] * 1000.0);
        rt60High.set(room.rt[5] * 1000.0);
        distance.set(room.r0);
        criticalDistance.set(room.critical);
        firstReflection.set((room.first + room.predelay) * 1000.0);
        clarity.set(clarityNow);
        definition.set(definitionNow);
        inLeft.set(gainToDb(peakIn0));
        inRight.set(gainToDb(peakIn1));
        outLeft.set(gainToDb(peakOut0));
        outRight.set(gainToDb(peakOut1));
        roomLeft.set(gainToDb(peakRoom0));
        roomRight.set(gainToDb(peakRoom1));
        sinceHitShown.set(sinceHit / rate);
        frozenShown.set(frozenW > 0.5);
    }

    // --- the editor ---------------------------------------------------------------------------

    static constexpr double kW = 1080;
    static constexpr double kH = 760;
    static constexpr double planX = 32;  // the plan's area
    static constexpr double planY = 116;
    static constexpr double planW = 528;
    static constexpr double planH = 352;
    static constexpr double cutY = 492;  // the section's
    static constexpr double cutH = 84;
    static constexpr double chipY = 604;  // the materials' first row
    static constexpr double rx = 588;     // the right-hand column
    static constexpr double rw = 476;
    static constexpr double gx = 604;  // the reflections' graph
    static constexpr double gy = 112;
    static constexpr double gw = 444;
    static constexpr double gh = 240;
    static constexpr double dx0 = 604;  // the decay graph
    static constexpr double dy0 = 418;
    static constexpr double dw = 244;
    static constexpr double dh = 126;

    // The editor's own.
    Room shown;
    double sc = 10.0, ox = 0.0, oy = 0.0;  // the plan: pixels per metre, where the room's corner is
    double hsc = 10.0;                     // the section's pixels per metre, up
    bool held = false;                     // the layout held through a drag
    int dragging = 0;  // 1 a knob, 2 the source, 3 the listener, 4 the right wall, 5 the back wall,
                       // 6 the ceiling, 7 the decay graph, 8 the reflections (predelay)
    int dragKnob = -1, hotKnob = -1, hotStem = -1, hotChip = -1, hotThing = 0;
    double knobPos = 0.0, pressX = 0.0, pressY = 0.0, pressValue = 0.0;
    double hoverX = -1.0, hoverY = -1.0;
    double span = 0.3;  // seconds across the reflections' graph

    void editor(Editor& e) { e.size(kW, kH); }

    // The room as the parameters set it, for drawing.
    void showRoom() {
        shown.w = roomWidth;
        shown.d = roomDepth;
        shown.h = roomHeight;
        shown.walls = walls.index();
        shown.floor = floor.index();
        shown.ceiling = ceiling.index();
        shown.mic = mic.index();
        shown.scale = decay;
        shown.scatter = scatter;
        shown.predelay = predelay * 0.001;
        shown.sxp = sourceX;
        shown.syp = sourceY;
        shown.lxp = listenerX;
        shown.lyp = listenerY;
        shown.compute();
    }

    // The plan to scale, centred in its area; held still while a wall is
    // dragged, so the wall stays under the pointer.
    void placePlan() {
        if (held) return;
        sc = min((planW - 24.0) / shown.w, (planH - 24.0) / shown.d);
        ox = planX + 0.5 * (planW - shown.w * sc);
        oy = planY + 0.5 * (planH - shown.d * sc);
        hsc = min(sc, (cutH - 14.0) / shown.h);
    }
    double toX(double x) const { return ox + x * sc; }
    double toY(double y) const { return oy + y * sc; }
    double floorY() const { return cutY + cutH - 4.0; }
    double toZ(double z) const { return floorY() - z * hsc; }

    // --- materials -----------------------------------------------------------------------------

    int materialColour(int m) const {
        switch (m) {
            case 0:
                return 0xA49D8E;  // stone
            case 1:
                return 0x85827A;  // concrete
            case 2:
                return 0xA65F44;  // brick
            case 3:
                return 0xDCCFB4;  // plaster
            case 4:
                return 0xB07C43;  // wood
            case 5:
                return 0x93B6B9;  // glass
            case 6:
                return 0x9A4E4A;  // carpet
            case 7:
                return 0x7A4566;  // curtains
            case 8:
                return 0xC79A57;  // audience
            case 9:
                return 0x758660;  // acoustic
            default:
                return 0x6F8FAE;  // open
        }
    }
    void fillMaterial(Canvas& g, int m, double x, double y) const {
        switch (m) {
            case 0:
                g.fillText("STONE", x, y);
                return;
            case 1:
                g.fillText("CONCRETE", x, y);
                return;
            case 2:
                g.fillText("BRICK", x, y);
                return;
            case 3:
                g.fillText("PLASTER", x, y);
                return;
            case 4:
                g.fillText("WOOD", x, y);
                return;
            case 5:
                g.fillText("GLASS", x, y);
                return;
            case 6:
                g.fillText("CARPET", x, y);
                return;
            case 7:
                g.fillText("CURTAINS", x, y);
                return;
            case 8:
                g.fillText("AUDIENCE", x, y);
                return;
            case 9:
                g.fillText("ACOUSTIC", x, y);
                return;
            default:
                g.fillText("OPEN", x, y);
        }
    }

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
        g.fillStyle(lit ? colour : (hot ? 0x38352A : Theme::raised));
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
        g.lineWidth(r > 24 ? 6 : 5);
        g.beginPath();
        g.arc(x, y, r, start, start + sweep);
        g.stroke();
        const double at = start + sweep * clamp(norm, 0.0, 1.0);
        const double from = bipolar ? start + 0.5 * sweep : start;
        g.strokeStyle(colour);
        g.lineWidth(r > 24 ? 4.5 : 3.5);
        if (abs(at - from) > 0.001) {
            g.beginPath();
            if (at >= from)
                g.arc(x, y, r, from, at);
            else
                g.arc(x, y, r, at, from);
            g.stroke();
        }
        g.radialGradient(x, y - r * 0.25, 2, r - 4, hot ? 0x5B5444 : kKnobLight, kKnobDark);
        g.beginPath();
        g.circle(x, y, r - 7);
        g.fill();
        g.strokeStyle(Theme::text);
        g.lineWidth(2);
        g.beginPath();
        g.moveTo(x + cos(at) * (r - 15), y + sin(at) * (r - 15));
        g.lineTo(x + cos(at) * (r - 9), y + sin(at) * (r - 9));
        g.stroke();
        g.fillStyle(Theme::textFaint);
        g.boldFont(8.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(name, x, y - r - 11);
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

    // --- the mark -----------------------------------------------------------------------------

    // A ghyll: two rock walls with moss on their rims and a stream falling
    // between them, which brightens as the room sounds.
    void drawMark(Canvas& g, double x, double y) {
        const double glow = clamp((max(roomLeft.get(), roomRight.get()) + 48.0) / 48.0, 0.0, 1.0);
        g.lineJoin(LineJoin::Round);
        for (int side = -1; side <= 1; side += 2) {
            const double s = side;
            g.linearGradient(x, y - 12, x, y + 20, kBark, kGroundBottom);
            g.beginPath();
            g.moveTo(x + 19 * s, y + 20);
            g.lineTo(x + 17 * s, y - 4);
            g.quadraticCurveTo(x + 16 * s, y - 11 - s, x + 9 * s, y - 11 - s);
            g.lineTo(x + 6 * s, y - 3);
            g.lineTo(x + 7 * s, y + 6);
            g.lineTo(x + 3 * s, y + 20);
            g.closePath();
            g.fill();
            for (int k = 0; k < 3; ++k) {
                g.fillStyle(blend(imod(k, 2) == 0 ? kMoss : kLeaf, kSun, 0.3 * glow));
                g.beginPath();
                g.circle(x + (10.5 + 3 * k) * s, y - 11.5 - s, 2.4 - 0.3 * k);
                g.fill();
            }
        }
        const int water = blend(kSky, kFrost, glow);
        g.lineCap(LineCap::Round);
        g.globalAlpha(0.7 + 0.3 * glow);
        g.strokeStyle(water);
        g.lineWidth(2.4);
        g.beginPath();
        g.moveTo(x - 1, y - 9);
        g.bezierCurveTo(x + 3, y - 3, x - 3, y + 5, x + 0.5, y + 16);
        g.stroke();
        g.strokeStyle(kFrost);
        g.globalAlpha(0.35 + 0.5 * glow);
        g.lineWidth(0.8);
        g.beginPath();
        g.moveTo(x - 0.5, y - 7);
        g.bezierCurveTo(x + 2.5, y - 2, x - 2, y + 5, x + 1, y + 14);
        g.stroke();
        g.globalAlpha(0.7 + 0.3 * glow);
        g.strokeStyle(water);
        g.lineWidth(1.6);
        g.beginPath();
        g.moveTo(x - 3.5, y + 18.5);
        g.lineTo(x + 4.5, y + 18.5);
        g.stroke();
        g.globalAlpha(1.0);
    }

    void drawHeader(Canvas& g) {
        drawMark(g, 30, 26);
        g.fillStyle(Theme::text);
        g.serifFont(23);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Alphabetic);
        g.fillText("Ghyll", 56, 37);
        const double nameWidth = g.measureText("Ghyll");
        g.fillStyle(Theme::textMuted);
        g.serifItalicFont(13);
        const double v = shown.volume;
        const char* what = frozenShown.get() ? "a held room"
                           : v < 40.0        ? "a booth or small room"
                           : v < 400.0       ? "a room"
                           : v < 3000.0      ? "a large room"
                           : v < 25000.0     ? "a hall"
                                             : "a vast space";
        g.fillText(what, 64 + nameWidth, 37);
        const double w2 = g.measureText(what);
        g.fillStyle(Theme::textFaint);
        g.serifItalicFont(11);
        g.fillText(format("%.0f m³, %.2f s", v, shown.rtMid()), 74 + nameWidth + w2, 37);
        g.textBaseline(TextBaseline::Middle);
        drawLevel(g, 604, 23, 120, inLeft.get(), inRight.get(), "IN");
        drawLevel(g, 776, 23, 120, roomLeft.get(), roomRight.get(), "ROOM");
        drawLevel(g, 940, 23, 120, outLeft.get(), outRight.get(), "OUT");
    }

    // --- the plan -----------------------------------------------------------------------------

    // A surface's edge: its material's colour, thick, with a lighter inner line.
    void edge(Canvas& g, double x0, double y0, double x1, double y1, int m) {
        g.lineCap(LineCap::Round);
        if (m == 10) {
            // Open: a dashed line, the sound goes through.
            g.strokeStyle(materialColour(m));
            g.lineWidth(1.5);
            g.globalAlpha(0.7);
            const double len = sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
            const int n = max(1, toInt(len / 9.0));
            g.beginPath();
            for (int k = 0; k < n; k += 2) {
                const double a = k / (n * 1.0), b = min(1.0, (k + 1) / (n * 1.0));
                g.moveTo(x0 + (x1 - x0) * a, y0 + (y1 - y0) * a);
                g.lineTo(x0 + (x1 - x0) * b, y0 + (y1 - y0) * b);
            }
            g.stroke();
            g.globalAlpha(1.0);
            return;
        }
        g.strokeStyle(blend(materialColour(m), kInk, 0.35));
        g.lineWidth(6);
        g.beginPath();
        g.moveTo(x0, y0);
        g.lineTo(x1, y1);
        g.stroke();
        g.strokeStyle(materialColour(m));
        g.lineWidth(2.5);
        g.beginPath();
        g.moveTo(x0, y0);
        g.lineTo(x1, y1);
        g.stroke();
    }

    // A first-order path in the plan or section: source, bounce, listener.
    void ray(Canvas& g, double sx, double sy, double bx, double by, double lx, double ly, double db,
             int colour) {
        const double a = clamp((db + 30.0) / 30.0, 0.08, 1.0);
        g.strokeStyle(colour);
        g.globalAlpha(0.15 + 0.55 * a);
        g.lineWidth(1.0 + 1.2 * a);
        g.beginPath();
        g.moveTo(sx, sy);
        g.lineTo(bx, by);
        g.lineTo(lx, ly);
        g.stroke();
        g.fillStyle(colour);
        g.beginPath();
        g.circle(bx, by, 2.2);
        g.fill();
        g.globalAlpha(1.0);
    }

    // The first-order image off one surface, or -1.
    int imageOff(int axis, bool beyond) const {
        for (int i = 0; i < shown.images; ++i) {
            if (shown.order[i] != 1) continue;
            const double v = axis == 0 ? shown.ix[i] : (axis == 1 ? shown.iy[i] : shown.iz[i]);
            const double s = axis == 0 ? shown.sx : (axis == 1 ? shown.sy : shown.sz);
            if (abs(v - s) < 1e-9) continue;
            if ((v > s) == beyond) return i;
        }
        return -1;
    }

    void drawSource(Canvas& g, double x, double y) {
        const double since = sinceHitShown.get();
        const double pulse = since < 0.6 ? 1.0 - since / 0.6 : 0.0;
        for (int k = 0; k < 3; ++k) {
            g.strokeStyle(kSun);
            g.globalAlpha((0.35 - 0.1 * k) * (0.4 + 0.6 * pulse));
            g.lineWidth(1.2);
            g.beginPath();
            g.circle(x, y, 9.0 + 6.0 * k + 10.0 * (1.0 - pulse) * (since < 0.6 ? 1.0 : 0.0));
            g.stroke();
        }
        g.globalAlpha(1.0);
        g.radialGradient(x, y - 2, 1, 8, 0xF6E3B4, kSun);
        g.beginPath();
        g.circle(x, y, 6.5);
        g.fill();
        g.strokeStyle(kInk);
        g.lineWidth(1.5);
        g.stroke();
    }

    // The listener: the capsules (or a head) facing the source.
    void drawListener(Canvas& g, double x, double y, bool section) {
        const int m = mic.index();
        const double fx = section ? 0.0 : shown.fx, fy = section ? 0.0 : shown.fy;
        const double rxv = -fy, ryv = fx;
        if (m == 2 && !section) {
            g.fillStyle(kLinen);
            g.beginPath();
            g.circle(x, y, 7.0);
            g.fill();
            g.fillStyle(kLinen);
            g.beginPath();
            g.circle(x + fx * 7.0, y + fy * 7.0, 2.2);  // the nose
            g.fill();
            for (int s = -1; s <= 1; s += 2) {
                g.fillStyle(kBark);
                g.beginPath();
                g.circle(x + s * rxv * 7.0, y + s * ryv * 7.0, 2.4);
                g.fill();
            }
            return;
        }
        const double gap = m == 1 ? 9.0 : (m == 3 ? 0.0 : 4.0);
        for (int s = -1; s <= 1; s += 2) {
            if (m == 3 && s == 1) continue;
            const double cx0 = x + s * rxv * gap, cy0 = y + s * ryv * gap;
            const double ang = m == 0 && !section ? s * 0.96 : 0.0;
            const double ax0 = fx * cos(ang) + s * rxv * sin(ang) * 1.0;
            const double ay0 = fy * cos(ang) + s * ryv * sin(ang) * 1.0;
            g.fillStyle(kLinen);
            g.beginPath();
            g.circle(cx0, cy0, 4.2);
            g.fill();
            if (!section && m == 0) {
                g.strokeStyle(kLinen);
                g.lineWidth(1.6);
                g.beginPath();
                g.moveTo(cx0, cy0);
                g.lineTo(cx0 + ax0 * 9.0, cy0 + ay0 * 9.0);
                g.stroke();
            }
        }
    }

    void drawPlan(Canvas& g) {
        well(g, planX, planY, planW, planH);
        const double x0 = toX(0.0), y0 = toY(0.0), x1 = toX(shown.w), y1 = toY(shown.d);
        // The floor, its material faintly.
        g.fillStyle(materialColour(floor.index()));
        g.globalAlpha(0.09);
        g.fillRect(x0, y0, x1 - x0, y1 - y0);
        g.globalAlpha(1.0);
        // A metre grid, or five, inside.
        const double stepM =
            shown.w > 30.0 || shown.d > 30.0 ? 5.0 : (shown.w > 8.0 || shown.d > 8.0 ? 2.0 : 1.0);
        g.strokeStyle(kGrid);
        g.lineWidth(1);
        g.beginPath();
        for (double m = stepM; m < shown.w - 1e-6; m += stepM) {
            g.moveTo(toX(m), y0);
            g.lineTo(toX(m), y1);
        }
        for (double m = stepM; m < shown.d - 1e-6; m += stepM) {
            g.moveTo(x0, toY(m));
            g.lineTo(x1, toY(m));
        }
        g.stroke();
        // The critical distance about the source: inside it the direct sound
        // is the louder.
        const double sxp = toX(shown.sx), syp = toY(shown.sy), lxp = toX(shown.lx),
                     lyp = toY(shown.ly);
        g.save();
        g.clip(x0, y0, x1 - x0, y1 - y0);
        g.strokeStyle(kSun);
        g.globalAlpha(0.35);
        g.lineWidth(1);
        const double rc = shown.critical * sc;
        const int dashes = max(12, toInt(twoPi * rc / 7.0));
        g.beginPath();
        for (int k = 0; k < dashes; k += 2) {
            const double a0 = twoPi * k / dashes, a1 = twoPi * (k + 1) / dashes;
            g.moveTo(sxp + rc * cos(a0), syp + rc * sin(a0));
            g.arc(sxp, syp, rc, a0, a1);
        }
        g.stroke();
        g.fillStyle(kSun);
        g.globalAlpha(0.035);
        g.beginPath();
        g.circle(sxp, syp, rc);
        g.fill();
        g.globalAlpha(1.0);
        g.restore();
        // The first-order paths off the four walls.
        for (int axis = 0; axis < 2; ++axis)
            for (int f = 0; f < 2; ++f) {
                const int i = imageOff(axis, f == 1);
                if (i < 0 || walls.index() == 10) continue;
                Array<double, 5> px, py, pz;
                const int n = shown.path(i, px, py, pz);
                if (n < 3) continue;
                ray(g, sxp, syp, toX(px[1]), toY(py[1]), lxp, lyp, shown.shownDb[i], kLinen);
            }
        // The hovered reflection's path, bounce by bounce.
        if (hotStem >= 0) drawPath(g, hotStem, false);
        // The direct path and its length.
        g.strokeStyle(kSun);
        g.globalAlpha(0.8);
        g.lineWidth(1.5);
        g.beginPath();
        g.moveTo(sxp, syp);
        g.lineTo(lxp, lyp);
        g.stroke();
        g.globalAlpha(1.0);
        // The walls.
        const int wm = walls.index();
        edge(g, x0, y0, x1, y0, wm);
        edge(g, x1, y0, x1, y1, wm);
        edge(g, x1, y1, x0, y1, wm);
        edge(g, x0, y1, x0, y0, wm);
        if (hotThing == 4 || dragging == 4) handle(g, x1, 0.5 * (y0 + y1), true);
        if (hotThing == 5 || dragging == 5) handle(g, 0.5 * (x0 + x1), y1, false);
        drawSource(g, sxp, syp);
        drawListener(g, lxp, lyp, false);
        label(g, "SOURCE", sxp, syp + (syp < lyp ? -22 : 22), TextAlign::Center);
        label(g, "LISTENER", lxp, lyp + (lyp <= syp ? -18 : 18), TextAlign::Center);
        // Sizes along the walls; the front wall named.
        g.fillStyle(Theme::textMuted);
        g.monoFont(10);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Bottom);
        g.fillText(format("%.1f m", shown.w), 0.5 * (x0 + x1), y0 - 6);
        g.save();
        g.translate(x1 + 8, 0.5 * (y0 + y1));
        g.rotate(0.5 * pi);
        g.fillText(format("%.1f m", shown.d), 0, 0);
        g.restore();
        label(g, "FRONT", x0, y0 - 11, TextAlign::Left);
        // The distance on the direct path.
        const double mxp = 0.5 * (sxp + lxp), myp = 0.5 * (syp + lyp);
        g.fillStyle(kInk);
        g.globalAlpha(0.75);
        g.beginPath();
        g.roundRect(mxp - 25, myp - 8, 50, 16, 8);
        g.fill();
        g.globalAlpha(1.0);
        g.fillStyle(kSun);
        g.monoFont(9.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(format("%.1f m", shown.r0), mxp, myp);
    }

    // A wall's grip, while the pointer is on it.
    void handle(Canvas& g, double x, double y, bool upright) {
        g.fillStyle(kLinen);
        g.beginPath();
        if (upright)
            g.roundRect(x - 3, y - 14, 6, 28, 3);
        else
            g.roundRect(x - 14, y - 3, 28, 6, 3);
        g.fill();
    }

    // An image's whole path, bounce by bounce, in the plan (or the section).
    void drawPath(Canvas& g, int i, bool section) {
        Array<double, 5> px, py, pz;
        const int n = shown.path(i, px, py, pz);
        g.strokeStyle(orderColour(shown.order[i]));
        g.lineWidth(2);
        g.lineJoin(LineJoin::Round);
        g.beginPath();
        for (int k = 0; k < n; ++k) {
            const double x = toX(px[k]), y = section ? toZ(pz[k]) : toY(py[k]);
            if (k == 0)
                g.moveTo(x, y);
            else
                g.lineTo(x, y);
        }
        g.stroke();
        g.fillStyle(orderColour(shown.order[i]));
        for (int k = 1; k + 1 < n; ++k) {
            g.beginPath();
            g.circle(toX(px[k]), section ? toZ(pz[k]) : toY(py[k]), 3);
            g.fill();
        }
    }

    // The section across the room: its height, the floor and the ceiling.
    void drawSection(Canvas& g) {
        well(g, planX, cutY, planW, cutH);
        const double x0 = toX(0.0), x1 = toX(shown.w);
        const double fy = floorY(), cy = toZ(shown.h);
        g.fillStyle(materialColour(walls.index()));
        g.globalAlpha(0.07);
        g.fillRect(x0, cy, x1 - x0, fy - cy);
        g.globalAlpha(1.0);
        const double sxp = toX(shown.sx), szp = toZ(shown.sz), lxp = toX(shown.lx),
                     lzp = toZ(shown.lz);
        for (int f = 0; f < 2; ++f) {
            const int i = imageOff(2, f == 1);
            const int m = f == 1 ? ceiling.index() : floor.index();
            if (i < 0 || m == 10) continue;
            Array<double, 5> px, py, pz;
            const int n = shown.path(i, px, py, pz);
            if (n < 3) continue;
            ray(g, sxp, szp, toX(px[1]), toZ(pz[1]), lxp, lzp, shown.shownDb[i], kLinen);
        }
        if (hotStem >= 0) drawPath(g, hotStem, true);
        edge(g, x0, fy, x1, fy, floor.index());
        edge(g, x0, cy, x1, cy, ceiling.index());
        edge(g, x0, fy, x0, cy, walls.index());
        edge(g, x1, fy, x1, cy, walls.index());
        if (hotThing == 6 || dragging == 6) handle(g, 0.5 * (x0 + x1), cy, false);
        g.strokeStyle(kSun);
        g.globalAlpha(0.5);
        g.lineWidth(1.2);
        g.beginPath();
        g.moveTo(sxp, szp);
        g.lineTo(lxp, lzp);
        g.stroke();
        g.globalAlpha(1.0);
        g.fillStyle(kSun);
        g.beginPath();
        g.circle(sxp, szp, 4.5);
        g.fill();
        drawListener(g, lxp, lzp, true);
        g.fillStyle(Theme::textMuted);
        g.monoFont(10);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(format("%.1f m", shown.h), x1 + 8, 0.5 * (fy + cy));
        label(g, "SECTION", planX + 8, cutY + 10, TextAlign::Left);
    }

    void drawMaterials(Canvas& g) {
        for (int row = 0; row < 3; ++row) {
            const int now = row == 0 ? walls.index() : (row == 1 ? floor.index() : ceiling.index());
            const double y = chipY + row * 46;
            label(g, row == 0 ? "WALLS" : (row == 1 ? "FLOOR" : "CEILING"), planX, y,
                  TextAlign::Left);
            // What it absorbs, low, mid and high: the hovered chip's or the chosen one's.
            const int show = hotChip >= 0 && idiv(hotChip, 16) == row ? imod(hotChip, 16) : now;
            g.fillStyle(Theme::textFaint);
            g.monoFont(9);
            g.textAlign(TextAlign::Right);
            g.textBaseline(TextBaseline::Middle);
            g.fillText(format("absorbs %.0f%% low, %.0f%% mid, %.0f%% high",
                              100.0 * bandAbsorption(show, 0), 100.0 * bandAbsorption(show, 1),
                              100.0 * bandAbsorption(show, 2)),
                       planX + planW, y);
            for (int m = 0; m < 11; ++m) {
                const double x = chipX(m);
                const bool lit = m == now;
                const bool hot = hotChip == row * 16 + m;
                g.fillStyle(lit ? materialColour(m) : (hot ? 0x38352A : Theme::raised));
                g.beginPath();
                g.roundRect(x, y + 8, 44, 24, 5);
                g.fill();
                if (!lit) {
                    g.strokeStyle(Theme::line);
                    g.lineWidth(1);
                    g.stroke();
                    // Its colour, a sliver.
                    g.fillStyle(materialColour(m));
                    g.globalAlpha(hot ? 0.9 : 0.6);
                    g.fillRect(x + 6, y + 27, 32, 2);
                    g.globalAlpha(1.0);
                }
                g.fillStyle(lit ? kInk : (hot ? Theme::text : Theme::textMuted));
                g.boldFont(7);
                g.textAlign(TextAlign::Center);
                g.textBaseline(TextBaseline::Middle);
                fillMaterial(g, m, x + 22, y + 19);
            }
        }
    }
    double chipX(int m) const { return planX + m * 48.0; }

    // --- the reflections ------------------------------------------------------------------------

    int orderColour(int k) const { return k == 1 ? kLinen : (k == 2 ? kLeaf : kMoss); }
    double stemTime(int i) const { return 0.5 * (shown.delay[i] + shown.delay[62 + i]); }
    double timeX(double t) const { return gx + 34.0 + (gw - 44.0) * t / span; }
    double levelY(double db) const { return gy + 12.0 + (gh - 34.0) * clamp(-db / 48.0, 0.0, 1.0); }

    void drawReflections(Canvas& g) {
        panel(g, rx, 72, rw, 300);
        heading(g, "Reflections", rx + 16, 92);
        // The seat's acoustics, as an acoustician would give them.
        g.fillStyle(Theme::textMuted);
        g.monoFont(9.5);
        g.textAlign(TextAlign::Right);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(format("first %.1f ms   C80 %+.1f dB   D50 %.0f%%",
                          (shown.first + shown.predelay) * 1000.0,
                          clamp(shown.clarity(), -30.0, 30.0), 100.0 * shown.definition()),
                   rx + rw - 16, 92);
        well(g, gx, gy, gw, gh);
        // The span: the paths, and a little of the tail.
        double last = 0.0;
        for (int i = 0; i < shown.images; ++i) last = max(last, stemTime(i));
        const double want = clamp(1.15 * last, 0.04, 1.6);
        span = want;
        // Grid: time along, every so many ms.
        const double stepS =
            span > 0.8 ? 0.2
                       : (span > 0.4 ? 0.1 : (span > 0.15 ? 0.05 : (span > 0.06 ? 0.02 : 0.01)));
        g.strokeStyle(kGrid);
        g.lineWidth(1);
        g.beginPath();
        for (double t = stepS; t < span; t += stepS) {
            g.moveTo(timeX(t), gy + 6);
            g.lineTo(timeX(t), gy + gh - 20);
        }
        for (int db = -12; db >= -36; db -= 12) {
            g.moveTo(gx + 30, levelY(db));
            g.lineTo(gx + gw - 6, levelY(db));
        }
        g.stroke();
        g.fillStyle(Theme::textFaint);
        g.monoFont(8.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        for (double t = stepS; t < span - 0.5 * stepS; t += stepS)
            g.fillText(format("%.0f", t * 1000.0), timeX(t), gy + gh - 10);
        g.textAlign(TextAlign::Left);
        g.fillText("ms", gx + gw - 22, gy + gh - 10);
        for (int db = -12; db >= -36; db -= 12) g.fillText(format("%d", db), gx + 8, levelY(db));
        // The tail: from the first-order reflections on, falling at each
        // octave's rate (low, mid, high), its level the order-three paths'.
        double third = 0.0, n3 = 0.0;
        for (int i = 0; i < shown.images; ++i)
            if (shown.order[i] == 3) {
                third += dbToGain(shown.shownDb[i]) * dbToGain(shown.shownDb[i]);
                n3 += 1.0;
            }
        const double startDb = n3 > 0.0 ? gainToDb(sqrt(third / n3)) : -24.0;
        const double t0 = shown.firstOrder + shown.predelay;
        const double lateDb = late.value() <= -29.99 ? -96.0 : late.value();
        for (int b = 0; b < 3; ++b) {
            const double rtb = b == 0 ? shown.rt[0] : (b == 1 ? shown.rtMid() : shown.rt[5]);
            const double y0 = levelY(startDb + lateDb),
                         y1 = levelY(startDb + lateDb - 60.0 * (span - t0) / rtb);
            if (b == 1) {
                g.linearGradient(0, y0, 0, gy + gh - 20, kLeaf, kGraphBottom);
                g.globalAlpha(0.22);
                g.beginPath();
                g.moveTo(timeX(t0), gy + gh - 20);
                g.lineTo(timeX(t0), y0);
                g.lineTo(timeX(span), y1);
                g.lineTo(timeX(span), gy + gh - 20);
                g.closePath();
                g.fill();
                g.globalAlpha(1.0);
            }
            g.strokeStyle(b == 0 ? kBark : (b == 1 ? kLeaf : kSky));
            g.lineWidth(b == 1 ? 1.8 : 1.2);
            g.globalAlpha(b == 1 ? 0.9 : 0.6);
            g.beginPath();
            g.moveTo(timeX(t0), y0);
            g.lineTo(timeX(span), y1);
            g.stroke();
            g.globalAlpha(1.0);
        }
        // The playhead from the last sound in; each path lights as it passes.
        const double since = sinceHitShown.get();
        const double earlyDb = early.value() <= -29.99 ? -96.0 : early.value();
        for (int k = 3; k >= 1; --k)
            for (int i = 0; i < shown.images; ++i) {
                if (shown.order[i] != k) continue;
                const double t = stemTime(i), x = timeX(t);
                const double top = levelY(shown.shownDb[i] + earlyDb);
                const bool lit = since >= t && since < t + 0.06;
                const bool hot = i == hotStem;
                g.strokeStyle(hot ? kSun : (lit ? 0xFFF4DA : orderColour(k)));
                g.globalAlpha(hot || lit ? 1.0 : (k == 1 ? 0.95 : (k == 2 ? 0.75 : 0.5)));
                g.lineWidth(hot ? 2.4 : (k == 1 ? 2.0 : 1.4));
                g.beginPath();
                g.moveTo(x, gy + gh - 20);
                g.lineTo(x, top);
                g.stroke();
                g.fillStyle(hot ? kSun : orderColour(k));
                g.beginPath();
                g.circle(x, top, k == 1 ? 2.6 : 1.8);
                g.fill();
            }
        g.globalAlpha(1.0);
        if (since < span) {
            const double x = timeX(since);
            g.strokeStyle(kSun);
            g.globalAlpha(0.6);
            g.lineWidth(1);
            g.beginPath();
            g.moveTo(x, gy + 6);
            g.lineTo(x, gy + gh - 20);
            g.stroke();
            g.globalAlpha(1.0);
        }
        // The predelay and the first reflection.
        if (shown.predelay > 0.0005) {
            g.fillStyle(kSun);
            g.globalAlpha(0.08);
            g.fillRect(timeX(0.0), gy + 6, timeX(shown.predelay) - timeX(0.0), gh - 26);
            g.globalAlpha(1.0);
        }
        // What the hovered path is, or the key.
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        if (hotStem >= 0) {
            const int i = hotStem;
            g.fillStyle(Theme::text);
            g.monoFont(10);
            const double used = describe(g, i, gx + 12, gy + 14);
            g.fillStyle(Theme::textMuted);
            g.fillText(format("%.1f ms, %.1f dB", stemTime(i) * 1000.0, shown.shownDb[i]),
                       gx + 24 + used, gy + 14);
        } else {
            key(g, gx + 12, gy + 14, kLinen, "1st");
            key(g, gx + 52, gy + 14, kLeaf, "2nd");
            key(g, gx + 94, gy + 14, kMoss, "3rd");
            key(g, gx + 136, gy + 14, kBark, "low");
            key(g, gx + 172, gy + 14, kLeaf, "mid");
            key(g, gx + 208, gy + 14, kSky, "high");
        }
    }

    void key(Canvas& g, double x, double y, int colour, const char* text) {
        g.fillStyle(colour);
        g.beginPath();
        g.circle(x + 3, y, 3);
        g.fill();
        g.fillStyle(Theme::textFaint);
        g.monoFont(9);
        g.textAlign(TextAlign::Left);
        g.fillText(text, x + 9, y);
    }

    // The surfaces a path met, in words, at x; the text's width.
    double describe(Canvas& g, int i, double x, double y) const {
        const int w = shown.onWalls[i], f = shown.onFloor[i], c = shown.onCeiling[i];
        const int k = shown.order[i];
        const char* text = "a wall, the floor and the ceiling";
        if (k == 1)
            text = w == 1 ? "a wall" : (f == 1 ? "the floor" : "the ceiling");
        else if (w == k)
            text = k == 2 ? "two walls" : "three walls";
        else if (f + c == k)
            text = k == 2 ? "the floor and the ceiling" : "floor, ceiling and floor again";
        else if (w == 1 && f == 1 && c == 0)
            text = "a wall and the floor";
        else if (w == 1 && c == 1 && f == 0)
            text = "a wall and the ceiling";
        else if (w == 2 && f == 1)
            text = "two walls and the floor";
        else if (w == 2 && c == 1)
            text = "two walls and the ceiling";
        g.fillText(text, x, y);
        return g.measureText(text);
    }

    int stemAt(double x, double y) const {
        if (x < gx || x > gx + gw || y < gy || y > gy + gh) return -1;
        int best = -1;
        double off = 5.0;
        for (int i = 0; i < shown.images; ++i) {
            const double d = abs(timeX(stemTime(i)) - x);
            if (d < off && y >= levelY(shown.shownDb[i]) - 8) {
                off = d;
                best = i;
            }
        }
        return best;
    }

    // --- decay ----------------------------------------------------------------------------------

    double hzX(double hz) const {
        return dx0 + 12.0 + (dw - 24.0) * log2(hz / 22.0) / log2(11000.0 / 22.0);
    }
    double rtTop() const {
        double t = 0.0;
        for (int b = 0; b < 7; ++b) t = max(t, shown.rt[b]);
        return t <= 0.5
                   ? 0.5
                   : (t <= 1.0
                          ? 1.0
                          : (t <= 2.0
                                 ? 2.0
                                 : (t <= 5.0 ? 5.0
                                             : (t <= 10.0 ? 10.0 : (t <= 30.0 ? 30.0 : 100.0)))));
    }
    double rtY(double t) const {
        return dy0 + dh - 18.0 - (dh - 30.0) * clamp(t / rtTop(), 0.0, 1.05);
    }

    void drawDecay(Canvas& g) {
        panel(g, rx, 384, rw, 176);
        heading(g, "Decay", rx + 16, 404);
        aside(g, "each octave's reverberation time", dx0 + dw, 404, TextAlign::Right);
        well(g, dx0, dy0, dw, dh);
        g.strokeStyle(kGrid);
        g.lineWidth(1);
        g.beginPath();
        for (int b = -2; b < 7; ++b) {
            g.moveTo(hzX(octaveHz(b)), dy0 + 6);
            g.lineTo(hzX(octaveHz(b)), dy0 + dh - 18);
        }
        g.moveTo(dx0 + 6, rtY(0.5 * rtTop()));
        g.lineTo(dx0 + dw - 6, rtY(0.5 * rtTop()));
        g.stroke();
        g.fillStyle(Theme::textFaint);
        g.monoFont(8.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        for (int b = -2; b < 7; b += 1) {
            const double hz = octaveHz(b);
            const double named = b == -2 ? 31.0 : (b == -1 ? 63.0 : hz);
            g.fillText(hz < 1000.0 ? format("%.0f", named) : format("%.0fk", hz * 0.001), hzX(hz),
                       dy0 + dh - 9);
        }
        g.textAlign(TextAlign::Left);
        g.textAlign(TextAlign::Right);
        if (rtTop() < 1.0) {
            g.fillText(format("%.1f s", rtTop()), dx0 + dw - 6, rtY(rtTop()) + 1);
            g.fillText(format("%.2f s", 0.5 * rtTop()), dx0 + dw - 6, rtY(0.5 * rtTop()) - 7);
        } else {
            g.fillText(format("%.0f s", rtTop()), dx0 + dw - 6, rtY(rtTop()) + 1);
            g.fillText(format("%.1f s", 0.5 * rtTop()), dx0 + dw - 6, rtY(0.5 * rtTop()) - 7);
        }
        // The modes: each a stem as tall as the listener hears it (30 dB).
        double loudest = 1e-12;
        for (int n = 0; n < shown.modes; ++n)
            loudest = max(loudest, abs(shown.modeGain[n]) + abs(shown.modeGain[48 + n]));
        if (shown.modes > 0) {
            const double xc = hzX(shown.crossover);
            g.strokeStyle(kBark);
            g.globalAlpha(0.35);
            g.lineWidth(1);
            g.beginPath();
            for (double y = dy0 + 8; y < dy0 + dh - 20; y += 6) {
                g.moveTo(xc, y);
                g.lineTo(xc, y + 3);
            }
            g.stroke();
            g.globalAlpha(1.0);
        }
        for (int n = 0; n < shown.modes; ++n) {
            if (shown.modeHz[n] < 22.0) continue;
            const double db =
                gainToDb((abs(shown.modeGain[n]) + abs(shown.modeGain[48 + n])) / loudest);
            const double t = clamp((db + 30.0) / 30.0, 0.0, 1.0);
            if (t <= 0.0) continue;
            const double x = hzX(shown.modeHz[n]), base = dy0 + dh - 18;
            g.strokeStyle(kBark);
            g.globalAlpha(0.35 + 0.6 * t);
            g.lineWidth(1.5);
            g.beginPath();
            g.moveTo(x, base);
            g.lineTo(x, base - 6 - 34 * t);
            g.stroke();
        }
        g.globalAlpha(1.0);
        // The curve and the area under it.
        g.linearGradient(0, dy0, 0, dy0 + dh, kLeaf, kGraphBottom);
        g.globalAlpha(0.25);
        g.beginPath();
        g.moveTo(hzX(octaveHz(0)), dy0 + dh - 18);
        for (int b = 0; b < 7; ++b) g.lineTo(hzX(octaveHz(b)), rtY(shown.rt[b]));
        g.lineTo(hzX(octaveHz(6)), dy0 + dh - 18);
        g.closePath();
        g.fill();
        g.globalAlpha(1.0);
        g.strokeStyle(dragging == 7 || hotThing == 7 ? kSun : kLeaf);
        g.lineWidth(2);
        g.lineJoin(LineJoin::Round);
        g.beginPath();
        for (int b = 0; b < 7; ++b) {
            if (b == 0)
                g.moveTo(hzX(octaveHz(b)), rtY(shown.rt[b]));
            else
                g.lineTo(hzX(octaveHz(b)), rtY(shown.rt[b]));
        }
        g.stroke();
        for (int b = 0; b < 7; ++b) {
            g.fillStyle(b == 0 ? kBark : (b == 5 ? kSky : kLeaf));
            g.beginPath();
            g.circle(hzX(octaveHz(b)), rtY(shown.rt[b]), b == 0 || b == 5 || b == 3 ? 3.2 : 2.2);
            g.fill();
        }
        // The three numbers.
        const double nx = dx0 + dw + 16;
        label(g, "LOW", nx, 428, TextAlign::Left);
        label(g, "MID", nx, 468, TextAlign::Left);
        label(g, "HIGH", nx, 508, TextAlign::Left);
        g.fillStyle(Theme::text);
        g.monoFont(11);
        g.textAlign(TextAlign::Left);
        fillSecondsLeft(g, shown.rt[0], nx, 442);
        g.monoFont(15);
        g.fillStyle(kLeaf);
        fillSecondsLeft(g, shown.rtMid(), nx, 484);
        g.monoFont(11);
        g.fillStyle(Theme::text);
        fillSecondsLeft(g, shown.rt[5], nx, 522);
        dial(g, knobX(0), knobY(0), knobR(0), decay.normalized(), kLeaf, false, hotKnob == 0,
             "DECAY");
        dial(g, knobX(1), knobY(1), knobR(1), scatter.normalized(), kMoss, false, hotKnob == 1,
             "SCATTER");
        valueStyle(g);
        g.fillText(format("%.0f%%", decay.value() * 100.0), knobX(0), knobY(0) + 36);
        g.fillText(format("%.0f%%", scatter.value() * 100.0), knobX(1), knobY(1) + 30);
        const bool fr = freeze.on();
        pill(g, 924, 524, 124, 22, fr ? "HELD: LET GO" : "FREEZE", fr, kFrost,
             hovered(924, 524, 124, 22));
    }

    void fillSecondsLeft(Canvas& g, double s, double x, double y) {
        if (s < 1.0)
            g.fillText(format("%.0f ms", s * 1000.0), x, y);
        else if (s < 10.0)
            g.fillText(format("%.2f s", s), x, y);
        else
            g.fillText(format("%.1f s", s), x, y);
    }

    // --- the sound panel
    // --------------------------------------------------------------------------

    void drawSound(Canvas& g) {
        panel(g, rx, 572, rw, 172);
        heading(g, "Listening", rx + 16, 592);
        aside(g, "how the room is heard, and how much of it", rx + rw - 16, 592, TextAlign::Right);
        for (int k = 0; k < 4; ++k) {
            const double x = 604 + k * 112;
            pill(g, x, 606, 104, 22,
                 k == 0 ? "ORTF PAIR" : (k == 1 ? "SPACED PAIR" : (k == 2 ? "HEAD" : "MONO")),
                 mic.index() == k, kLinen, hovered(x, 606, 104, 22));
        }
        dial(g, knobX(2), knobY(2), knobR(2), predelay.normalized(), kSun, false, hotKnob == 2,
             "PREDELAY");
        dial(g, knobX(3), knobY(3), knobR(3), early.normalized(), kLinen, false, hotKnob == 3,
             "EARLY");
        dial(g, knobX(4), knobY(4), knobR(4), late.normalized(), kLeaf, false, hotKnob == 4,
             "LATE");
        dial(g, knobX(5), knobY(5), knobR(5), lowCut.normalized(), kBark, false, hotKnob == 5,
             "LOW CUT");
        dial(g, knobX(6), knobY(6), knobR(6), 1.0 - highCut.normalized(), kSky, false, hotKnob == 6,
             "HIGH CUT");
        dial(g, knobX(7), knobY(7), knobR(7), width.normalized(), kLinen, false, hotKnob == 7,
             "WIDTH");
        dial(g, knobX(8), knobY(8), knobR(8), mix.normalized(), kLinen, false, hotKnob == 8, "MIX");
        dial(g, knobX(9), knobY(9), knobR(9), output.normalized(), kLinen, true, hotKnob == 9,
             "OUTPUT");
        valueStyle(g);
        g.monoFont(9.5);
        const double vy = knobY(2) + 31;
        g.fillText(predelay.value() < 0.5 ? "none" : format("%.0f ms", predelay.value()), knobX(2),
                   vy);
        if (early.value() <= -29.99)
            g.fillText("off", knobX(3), vy);
        else
            fillDb(g, early.value(), knobX(3), vy);
        if (late.value() <= -29.99)
            g.fillText("off", knobX(4), vy);
        else
            fillDb(g, late.value(), knobX(4), vy);
        if (lowCut.value() <= 20.5)
            g.fillText("off", knobX(5), vy);
        else
            fillHz(g, lowCut.value(), knobX(5), vy);
        if (highCut.value() >= 19500.0)
            g.fillText("off", knobX(6), vy);
        else
            fillHz(g, highCut.value(), knobX(6), vy);
        g.fillText(format("%.0f%%", width.value() * 100.0), knobX(7), vy);
        g.fillText(format("%.0f%%", mix.value() * 100.0), knobX(8), vy);
        fillDb(g, output.value(), knobX(9), vy);
    }

    void drawRoomPanel(Canvas& g) {
        panel(g, 16, 72, 560, 672);
        heading(g, "The room", 32, 92);
        aside(g, "drag the source, the listener and the walls; the wheel sizes the room", 560, 92,
              TextAlign::Right);
        drawPlan(g);
        drawSection(g);
        drawMaterials(g);
    }

    void draw(Canvas& g) {
        showRoom();
        placePlan();
        g.linearGradient(0, 0, 0, kH, kGroundTop, kGroundBottom);
        g.fillRect(0, 0, kW, kH);
        drawHeader(g);
        drawRoomPanel(g);
        drawReflections(g);
        drawDecay(g);
        drawSound(g);
    }

    // --- the knobs ----------------------------------------------------------------------------

    double knobX(int k) const {
        if (k == 0) return 962;
        if (k == 1) return 1030;
        return 630 + (k - 2) * 56;
    }
    double knobY(int k) const { return k <= 1 ? 466 : 690; }
    double knobR(int k) const { return k == 0 ? 25.0 : 19.0; }

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
                return decay.normalized();
            case 1:
                return scatter.normalized();
            case 2:
                return predelay.normalized();
            case 3:
                return early.normalized();
            case 4:
                return late.normalized();
            case 5:
                return lowCut.normalized();
            case 6:
                // The high cut turns the other way: clockwise closes it.
                return 1.0 - highCut.normalized();
            case 7:
                return width.normalized();
            case 8:
                return mix.normalized();
            default:
                return output.normalized();
        }
    }

    void setKnob(int k, int how, double v) {
        if (k == 0) act(how, decay, v);
        if (k == 1) act(how, scatter, v);
        if (k == 2) act(how, predelay, v);
        if (k == 3) act(how, early, v);
        if (k == 4) act(how, late, v);
        if (k == 5) act(how, lowCut, v);
        if (k == 6) act(how, highCut, how == 1 ? 1.0 - v : v);
        if (k == 7) act(how, width, v);
        if (k == 8) act(how, mix, v);
        if (k == 9) act(how, output, v);
    }

    int knobAt(double x, double y) const {
        for (int k = 0; k < 10; ++k) {
            const double ddx = x - knobX(k), ddy = y - knobY(k);
            const double r = knobR(k) + 6;
            if (ddx * ddx + ddy * ddy <= r * r) return k;
        }
        return -1;
    }

    // --- the pointer --------------------------------------------------------------------------

    // What in the room is under the pointer: 2 the source, 3 the listener,
    // 4 the right wall, 5 the back wall, 6 the ceiling, 7 the decay graph, 0
    // nothing.
    int thingAt(double x, double y) const {
        if (sqrt((x - toX(shown.sx)) * (x - toX(shown.sx)) +
                 (y - toY(shown.sy)) * (y - toY(shown.sy))) <= 11.0)
            return 2;
        if (sqrt((x - toX(shown.lx)) * (x - toX(shown.lx)) +
                 (y - toY(shown.ly)) * (y - toY(shown.ly))) <= 11.0)
            return 3;
        if (abs(x - toX(shown.w)) <= 6.0 && y > toY(0.0) && y < toY(shown.d)) return 4;
        if (abs(y - toY(shown.d)) <= 6.0 && x > toX(0.0) && x < toX(shown.w)) return 5;
        if (abs(y - toZ(shown.h)) <= 6.0 && x > toX(0.0) && x < toX(shown.w) && y > cutY &&
            y < cutY + cutH)
            return 6;
        if (x >= dx0 && x <= dx0 + dw && y >= dy0 && y <= dy0 + dh) return 7;
        return 0;
    }

    int chipAt(double x, double y) const {
        for (int row = 0; row < 3; ++row)
            for (int m = 0; m < 11; ++m)
                if (x >= chipX(m) && x <= chipX(m) + 44 && y >= chipY + row * 46 + 8 &&
                    y <= chipY + row * 46 + 32)
                    return row * 16 + m;
        return -1;
    }

    void pointer(const Pointer& p) {
        const PointerAction a = p.action();
        const double fine = p.shift() ? 0.2 : 1.0;
        showRoom();
        placePlan();
        if (a == PointerAction::Leave) {
            hoverX = -1.0;
            hoverY = -1.0;
            hotKnob = -1;
            hotStem = -1;
            hotChip = -1;
            hotThing = 0;
            return;
        }
        hoverX = p.x();
        hoverY = p.y();
        if (a == PointerAction::Move) {
            hotKnob = knobAt(p.x(), p.y());
            hotStem = stemAt(p.x(), p.y());
            hotChip = chipAt(p.x(), p.y());
            hotThing = thingAt(p.x(), p.y());
            return;
        }
        if (a == PointerAction::Press) {
            pressed(p);
            return;
        }
        if (a == PointerAction::Drag) {
            dragged(p, fine);
            return;
        }
        if (a == PointerAction::Release) {
            dragging = 0;
            held = false;
            hotKnob = knobAt(p.x(), p.y());
            hotThing = thingAt(p.x(), p.y());
            return;
        }
        if (a == PointerAction::Wheel) {
            const double notches = p.wheel();
            if (p.over(planX, planY, planW, cutY + cutH - planY)) {
                // The whole room larger or smaller, its shape kept.
                const double f = exp2(notches * 0.05 * fine);
                roomWidth.edit(roomWidth * f);
                roomDepth.edit(roomDepth * f);
                roomHeight.edit(roomHeight * f);
                return;
            }
            if (p.over(dx0, dy0, dw, dh)) {
                decay.edit(decay * exp2(notches * 0.05 * fine));
                return;
            }
            const int k = knobAt(p.x(), p.y());
            if (k >= 0) setKnob(k, 1, clamp(knobValue(k) + notches * 0.02 * fine, 0.0, 1.0));
        }
    }

    void pressed(const Pointer& p) {
        const double x = p.x(), y = p.y();
        pressX = x;
        pressY = y;
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
        const int t = thingAt(x, y);
        if (t > 0) {
            if (p.doubleClick()) {
                if (t == 2) {
                    sourceX.reset();
                    sourceY.reset();
                } else if (t == 3) {
                    listenerX.reset();
                    listenerY.reset();
                } else if (t == 4) {
                    roomWidth.reset();
                } else if (t == 5) {
                    roomDepth.reset();
                } else if (t == 6) {
                    roomHeight.reset();
                } else {
                    decay.reset();
                }
                return;
            }
            dragging = t;
            held = t >= 4 && t <= 6;
            pressValue = t == 4 ? roomWidth.value()
                                : (t == 5 ? roomDepth.value()
                                          : (t == 6 ? roomHeight.value() : decay.value()));
            return;
        }
        const int c = chipAt(x, y);
        if (c >= 0) {
            const int row = idiv(c, 16), m = imod(c, 16);
            if (row == 0) act(0, walls, m);
            if (row == 1) act(0, floor, m);
            if (row == 2) act(0, ceiling, m);
            return;
        }
        for (int m = 0; m < 4; ++m)
            if (p.over(604 + m * 112, 606, 104, 22)) {
                act(0, mic, m);
                return;
            }
        if (p.over(924, 524, 124, 22)) {
            act(0, freeze, freeze.on() ? 0 : 1);
            return;
        }
        if (p.over(gx, gy, gw, gh)) {
            if (p.doubleClick()) {
                predelay.reset();
                return;
            }
            dragging = 8;
            pressValue = predelay.value();
        }
    }

    void dragged(const Pointer& p, double fine) {
        const double x = p.x(), y = p.y();
        if (dragging == 1) {
            knobPos = clamp(knobPos - p.dy() * fine / 220.0, 0.0, 1.0);
            setKnob(dragKnob, 1, knobPos);
        } else if (dragging == 2) {
            sourceX.edit(clamp((x - ox) / sc / shown.w, 0.0, 1.0));
            sourceY.edit(clamp((y - oy) / sc / shown.d, 0.0, 1.0));
        } else if (dragging == 3) {
            listenerX.edit(clamp((x - ox) / sc / shown.w, 0.0, 1.0));
            listenerY.edit(clamp((y - oy) / sc / shown.d, 0.0, 1.0));
        } else if (dragging == 4) {
            roomWidth.edit(pressValue + (x - pressX) * fine / sc);
        } else if (dragging == 5) {
            roomDepth.edit(pressValue + (y - pressY) * fine / sc);
        } else if (dragging == 6) {
            roomHeight.edit(pressValue - (y - pressY) * fine / hsc);
        } else if (dragging == 7) {
            decay.edit(pressValue * exp2(-(y - pressY) * fine / 60.0));
        } else if (dragging == 8) {
            predelay.edit(pressValue + (x - pressX) * fine / (gw - 44.0) * span * 1000.0);
        }
    }
};
