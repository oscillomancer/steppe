// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Arithmetic on two signals, poly: their sum, difference and product (ring
// modulation, or one CV scaling another), the lower and the higher of the
// two, A's size without its sign, A crossfaded to B by Blend (a cable into
// Blend makes it a voltage-controlled crossfader), and a comparator: high
// while A is above B, with a little hysteresis so a noisy crossing does not
// chatter. B is 0 with nothing in it, so the comparator alone tells when A
// goes positive.

struct Math : Module {
    static constexpr ModuleInfo info = {
        .name = "Math",
        .id = "com.suppleaudio.rack.math",
        .vendor = "Supple Audio",
        .description = "Sum, difference, product, min, max, abs, a crossfade and a comparator.",
        .tags = {"utility", "logic", "poly"}};

    Param blend =
        percent("Blend", 50)
            .smoothed(5)
            .help("Where Blend sits between A (0 %) and B (100 %); a cable makes it a crossfader.");
    Param hysteresis =
        continuous("Hysteresis", 0, 0.5, 0.01)
            .help("How far A must cross B for Greater to change: more ignores noise.");

    Input a = cvIn("A").help("The first signal, poly.");
    Input b = cvIn("B").help("The second signal, poly; 0 with nothing here.");

    Output sum = cvOut("Sum").help("A plus B.");
    Output difference = cvOut("Difference").help("A minus B.");
    Output product = cvOut("Product").help("A times B.");
    Output minimum = cvOut("Minimum").help("The lower of A and B.");
    Output maximum = cvOut("Maximum").help("The higher of A and B.");
    Output absolute = cvOut("Absolute").help("A without its sign: a full-wave rectifier.");
    Output mix = cvOut("Mix").help("A crossfaded to B by Blend.");
    Output greater = gateOut("Greater").help("High while A is above B.");

    Array<bool, 16> above;

    void process(int frames) {
        for (int c = 0; c < channels(); ++c)
            for (int i = 0; i < frames; ++i) {
                const double x = a[c][i];
                const double y = b[c][i];
                const double h = 0.5 * max(0.0, hysteresis.at(i, c));
                if (!above[c] && x > y + h) above[c] = true;
                if (above[c] && x < y - h) above[c] = false;
                const double t = clamp(blend.at(i, c), 0.0, 1.0);
                sum[c][i] = x + y;
                difference[c][i] = x - y;
                product[c][i] = x * y;
                minimum[c][i] = min(x, y);
                maximum[c][i] = max(x, y);
                absolute[c][i] = abs(x);
                mix[c][i] = x + (y - x) * t;
                greater[c][i] = above[c] ? 1.0 : 0.0;
            }
    }
};
