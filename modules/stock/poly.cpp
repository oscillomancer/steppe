// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Poly tools in one: Merge gathers up to eight mono signals into one poly
// cable (as many channels as the last input connected, or Channels when
// set); Split deals a poly cable's first eight channels out to mono jacks;
// Sum adds its channels to one (or averages them); Spread pans them across
// a stereo pair, the first channel left and the last right, as far apart
// as Spread says: a poly voice section into a wide stereo image.

struct Poly : Module {
    static constexpr ModuleInfo info = {.name = "Poly",
                                        .id = "com.suppleaudio.rack.poly",
                                        .vendor = "Supple Audio",
                                        .description = "Merge, split, sum and spread poly cables.",
                                        .tags = {"utility", "poly"}};

    Param mergeChannels =
        integer("Channels", 0, 8, 0)
            .withKey("channels")
            .help("Merged's channels; 0 takes as many as the last input connected.");
    Param average = toggle("Average", false).help("Sum divides by the channels: an average.");
    Param spread = percent("Spread", 100)
                       .smoothed(10)
                       .help(
                           "How far apart Spread places the channels: 0 % all centred, 100 % from "
                           "hard left to "
                           "hard right.");

    Input merge_1 = cvIn("Merge 1").help("Channel 1 of Merged.");
    Input merge_2 = cvIn("Merge 2").help("Channel 2 of Merged.");
    Input merge_3 = cvIn("Merge 3").help("Channel 3 of Merged.");
    Input merge_4 = cvIn("Merge 4").help("Channel 4 of Merged.");
    Input merge_5 = cvIn("Merge 5").help("Channel 5 of Merged.");
    Input merge_6 = cvIn("Merge 6").help("Channel 6 of Merged.");
    Input merge_7 = cvIn("Merge 7").help("Channel 7 of Merged.");
    Input merge_8 = cvIn("Merge 8").help("Channel 8 of Merged.");
    Input polyIn = cvIn("Poly In").help("The poly cable to split, sum and spread.");

    Output merged = cvOut("Merged").help("The merge inputs, a channel each.");
    Output split_1 = cvOut("Split 1").help("Channel 1 of Poly In, alone.");
    Output split_2 = cvOut("Split 2").help("Channel 2 of Poly In, alone.");
    Output split_3 = cvOut("Split 3").help("Channel 3 of Poly In, alone.");
    Output split_4 = cvOut("Split 4").help("Channel 4 of Poly In, alone.");
    Output split_5 = cvOut("Split 5").help("Channel 5 of Poly In, alone.");
    Output split_6 = cvOut("Split 6").help("Channel 6 of Poly In, alone.");
    Output split_7 = cvOut("Split 7").help("Channel 7 of Poly In, alone.");
    Output split_8 = cvOut("Split 8").help("Channel 8 of Poly In, alone.");
    Output sum = cvOut("Sum").help("Poly In's channels added (or averaged).");
    Output left = audioOut("Left").help("Poly In spread across the stereo pair: the left.");
    Output right = audioOut("Right").help("Poly In spread across the stereo pair: the right.");

    void process(int frames) {
        int last = 1;
        if (merge_1.connected()) last = 1;
        if (merge_2.connected()) last = 2;
        if (merge_3.connected()) last = 3;
        if (merge_4.connected()) last = 4;
        if (merge_5.connected()) last = 5;
        if (merge_6.connected()) last = 6;
        if (merge_7.connected()) last = 7;
        if (merge_8.connected()) last = 8;
        const int set = mergeChannels.index();
        const int n = set > 0 ? set : last;
        merged.setChannels(n);
        split_1.setChannels(1);
        split_2.setChannels(1);
        split_3.setChannels(1);
        split_4.setChannels(1);
        split_5.setChannels(1);
        split_6.setChannels(1);
        split_7.setChannels(1);
        split_8.setChannels(1);
        sum.setChannels(1);
        left.setChannels(1);
        right.setChannels(1);
        const int m = max(1, polyIn.channels());
        for (int i = 0; i < frames; ++i) {
            if (0 < n) merged[0][i] = merge_1[0][i];
            if (1 < n) merged[1][i] = merge_2[0][i];
            if (2 < n) merged[2][i] = merge_3[0][i];
            if (3 < n) merged[3][i] = merge_4[0][i];
            if (4 < n) merged[4][i] = merge_5[0][i];
            if (5 < n) merged[5][i] = merge_6[0][i];
            if (6 < n) merged[6][i] = merge_7[0][i];
            if (7 < n) merged[7][i] = merge_8[0][i];
            split_1[0][i] = polyIn[0][i];
            split_2[0][i] = polyIn[1][i];
            split_3[0][i] = polyIn[2][i];
            split_4[0][i] = polyIn[3][i];
            split_5[0][i] = polyIn[4][i];
            split_6[0][i] = polyIn[5][i];
            split_7[0][i] = polyIn[6][i];
            split_8[0][i] = polyIn[7][i];
            double s = 0.0;
            double l = 0.0;
            double r = 0.0;
            const double width = clamp(spread.at(i), 0.0, 1.0);
            for (int c = 0; c < m; ++c) {
                const double x = polyIn[c][i];
                s += x;
                const double place = m > 1 ? (2.0 * c / (m - 1) - 1.0) * width : 0.0;
                const double angle = (place + 1.0) * pi * 0.25;
                l += x * cos(angle);
                r += x * sin(angle);
            }
            sum[0][i] = average.on() ? s / m : s;
            left[0][i] = l;
            right[0][i] = r;
        }
    }
};
