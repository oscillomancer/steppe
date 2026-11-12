// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <supple/base/rng.h>
#include <supple/sync/mtc.h>

#include <cmath>
#include <vector>

using namespace supple;
using namespace supple::sync;

namespace {

constexpr double kRate = 48000.0;

struct Collected {
    std::vector<MidiEvent> storage = std::vector<MidiEvent>(1 << 16);
    EventBuffer buffer{storage};
};

Timecode tc(int h, int m, int s, int f) { return Timecode{h, m, s, f}; }

// Re-assembles the label that a run of eight forward quarter frames names.
Timecode assemble(const MidiEvent* e) {
    int n[8];
    for (int p = 0; p < 8; ++p) n[p] = e[p].bytes[1] & 0x0F;
    return tc(n[6] | ((n[7] & 1) << 4), n[4] | (n[5] << 4), n[2] | (n[3] << 4), n[0] | (n[1] << 4));
}

void run(MtcGenerator& g, const PositionMap& map, double from, double to, double block,
         EventBuffer& out) SUPPLE_RT {
    for (double f = from; f < to; f += block) g.run(f, std::min(f + block, to), map, out);
}

}  // namespace

TEST_CASE("mtc: quarter frames at exact frames, in piece order") {
    for (FrameRate rate :
         {FrameRate::Fps24, FrameRate::Fps25, FrameRate::Fps2997Drop, FrameRate::Fps30}) {
        CAPTURE(rateName(rate));
        Collected c;
        MtcGenerator g(TimecodeMapping{rate, tc(1, 0, 0, 0)});
        const LinearMap seconds(0.0, 0.0, 1.0 / kRate);
        g.start(0.0, seconds, c.buffer);
        run(g, seconds, 0.0, 2.0 * kRate, 333.0, c.buffer);
        const auto e = c.buffer.events();
        REQUIRE(e.size() > 9);
        // The full frame first: F0 7F 7F 01 01 hh mm ss ff F7.
        CHECK(e[0].size == 10);
        CHECK(e[0].bytes[5] == ((mtcRateCode(rate) << 5) | 1));
        CHECK(e[0].bytes[6] == 0);
        CHECK(e[0].bytes[8] == 0);
        CHECK(e[0].bytes[9] == 0xF7);
        const double quarter = kRate / rateInfo(rate).fps() / 4.0;
        const std::int64_t first = toFrames(tc(1, 0, 0, 0), rate);
        for (std::size_t k = 1; k < e.size(); ++k) {
            CHECK(e[k].bytes[0] == 0xF1);
            CHECK((e[k].bytes[1] >> 4) == static_cast<int>((k - 1) % 8));
            CHECK(e[k].frame ==
                  doctest::Approx(static_cast<double>(k - 1) * quarter).epsilon(1e-12));
        }
        CHECK(e.size() - 1 == static_cast<std::size_t>(std::ceil(2.0 * kRate / quarter)));
        // Each sequence names its even frame; the rate code rides in piece 7.
        for (std::size_t s = 1; s + 8 <= e.size(); s += 8) {
            const auto frame = first + static_cast<std::int64_t>((s - 1) / 4);
            CHECK(assemble(&e[s]) == fromFrames(frame, rate));
            CHECK(((e[s + 7].bytes[1] >> 1) & 3) == mtcRateCode(rate));
        }
    }
}

TEST_CASE("mtc: starting inside a frame waits for the next even frame") {
    Collected c;
    MtcGenerator g(TimecodeMapping{FrameRate::Fps25, tc(0, 0, 0, 0)});
    // Timeline 1.05 s: frame 26.25, so the first sequence is frame 28.
    const LinearMap seconds(0.0, 1.05, 1.0 / kRate);
    g.start(0.0, seconds, c.buffer);
    CHECK(g.nextQuarter() == 28 * 4);
    g.run(0.0, kRate, seconds, c.buffer);
    const auto e = c.buffer.events();
    CHECK(e[0].bytes[8] == 1);  // full frame 00:00:01:01, the frame playing
    CHECK(e[0].bytes[7] == 1);
    CHECK(e[1].frame == doctest::Approx((28.0 / 25.0 - 1.05) * kRate));
    CHECK(assemble(&e[1]) == tc(0, 0, 1, 3));
}

TEST_CASE("mtc: drop-frame sequences across a minute boundary") {
    Collected c;
    const auto rate = FrameRate::Fps2997Drop;
    TimecodeMapping m{rate, tc(0, 0, 59, 20)};
    MtcGenerator g(m);
    const LinearMap seconds(0.0, 0.0, 1.0 / kRate);
    g.start(0.0, seconds, c.buffer);
    run(g, seconds, 0.0, 0.8 * kRate, 512.0, c.buffer);
    const auto e = c.buffer.events();
    std::vector<Timecode> labels;
    for (std::size_t s = 1; s + 8 <= e.size(); s += 8) labels.push_back(assemble(&e[s]));
    REQUIRE(labels.size() > 6);
    CHECK(labels[0] == tc(0, 0, 59, 20));
    CHECK(labels[4] == tc(0, 0, 59, 28));
    CHECK(labels[5] == tc(0, 1, 0, 2));  // :00 and :01 skipped
    CHECK(labels[6] == tc(0, 1, 0, 4));
}

TEST_CASE("mtc: locate sends a full frame and restarts the sequence") {
    Collected c;
    MtcGenerator g(TimecodeMapping{FrameRate::Fps30, tc(0, 0, 0, 0)});
    const LinearMap a(0.0, 0.0, 1.0 / kRate);
    g.start(0.0, a, c.buffer);
    g.run(0.0, 10000.0, a, c.buffer);
    const LinearMap b(10000.0, 100.0, 1.0 / kRate);
    const std::size_t mark = c.buffer.size();
    g.locate(10000.0, b, c.buffer);
    g.run(10000.0, 20000.0, b, c.buffer);
    const auto e = c.buffer.events();
    CHECK(e[mark].size == 10);
    CHECK(e[mark].bytes[6] == 1);  // 00:01:40:00
    CHECK(e[mark].bytes[7] == 40);
    CHECK(e[mark + 1].frame == 10000.0);
    CHECK(assemble(&e[mark + 1]) == tc(0, 1, 40, 0));
    g.stop();
    const std::size_t stopped = c.buffer.size();
    g.run(20000.0, 30000.0, b, c.buffer);
    CHECK(c.buffer.size() == stopped);
    CHECK(MtcGenerator::maxEventsPerBlock(48000.0, kRate, FrameRate::Fps30, 1.0) >= 121);
}

namespace {

struct LoopResult {
    double lockedAt = -1.0;
    double worst = 0.0;
    FrameRate rate = FrameRate::Fps25;
    bool rateMeasured = false;
    std::uint32_t jumps = 0;
};

// Generator -> jitter -> receiver for `seconds` of real time at `speed`.
LoopResult loop(FrameRate rate, MtcReceiverConfig config, double duration, double jitterMs,
                double speed = 1.0) {
    Collected c;
    TimecodeMapping m{rate, tc(10, 0, 0, 0)};
    MtcGenerator g(m);
    const LinearMap seconds(0.0, 0.0, speed / kRate);
    MtcReceiver rx(config);
    Rng rng(3);
    LoopResult r;
    const double jitter = jitterMs * 0.001 * kRate;
    for (double f = 0.0; f < duration * kRate; f += 480.0) {
        c.buffer.clear();
        if (f == 0.0) g.start(0.0, seconds, c.buffer);
        g.run(f, f + 480.0, seconds, c.buffer);
        for (const MidiEvent& e : c.buffer.events()) {
            const double at = e.frame + rng.uniform(-jitter, jitter);
            if (rx.feed(at, std::span(e.bytes.data(), e.size)) && rx.locked()) {
                if (r.lockedAt < 0) r.lockedAt = at;
                const Observation o = rx.lastObservation();
                // The position sent at that frame, in the sent rate's seconds.
                const double sent = toSeconds(m.framesAt(seconds.positionAt(o.frame)), rate);
                if (at > r.lockedAt + 0.5 * kRate)
                    r.worst = std::max(r.worst, std::abs(o.position - sent));
            }
        }
        rx.update(f + 480.0);
    }
    r.rate = rx.rate();
    r.rateMeasured = rx.rateMeasured();
    r.jumps = rx.jumps();
    return r;
}

}  // namespace

TEST_CASE("mtc: the receiver follows the generator through jitter") {
    for (FrameRate rate :
         {FrameRate::Fps24, FrameRate::Fps25, FrameRate::Fps2997Drop, FrameRate::Fps30}) {
        CAPTURE(rateName(rate));
        MtcReceiverConfig config{kRate};
        const LoopResult r = loop(rate, config, 4.0, 1.0);
        MESSAGE(rateName(rate), ": locked after ", r.lockedAt / kRate, " s, worst error ",
                r.worst * 1000, " ms");
        REQUIRE(r.lockedAt > 0);
        CHECK(r.lockedAt < 0.25 * kRate);  // two sequences and a bit
        CHECK(r.worst < 0.0006);           // filtered times within 0.6 ms
        CHECK(r.rate == rate);
        CHECK(r.jumps == 1);  // the full frame
    }
}

TEST_CASE("mtc: a dropout loses lock, which returns with the next sequences") {
    Collected c;
    TimecodeMapping m{FrameRate::Fps25, tc(1, 0, 0, 0)};
    MtcGenerator g(m);
    const LinearMap seconds(0.0, 0.0, 1.0 / kRate);
    MtcReceiver rx(MtcReceiverConfig{kRate});
    const double dropFrom = 1.0 * kRate;
    const double dropTo = 1.4 * kRate;
    bool lost = false;
    double relockedAt = -1.0;
    for (double f = 0.0; f < 3.0 * kRate; f += 480.0) {
        c.buffer.clear();
        if (f == 0.0) g.start(0.0, seconds, c.buffer);
        g.run(f, f + 480.0, seconds, c.buffer);
        for (const MidiEvent& e : c.buffer.events()) {
            if (e.frame >= dropFrom && e.frame < dropTo) continue;
            if (rx.feed(e.frame, std::span(e.bytes.data(), e.size)) && e.frame > dropTo &&
                rx.locked()) {
                if (relockedAt < 0) relockedAt = e.frame;
                const Observation o = rx.lastObservation();
                CHECK(
                    o.position ==
                    doctest::Approx(m.startSeconds() + seconds.positionAt(o.frame)).epsilon(1e-9));
            }
        }
        rx.update(f + 480.0);
        if (f > dropFrom && f < dropTo && !rx.present()) lost = true;
    }
    CHECK(lost);
    REQUIRE(relockedAt > 0);
    CHECK(relockedAt - dropTo < 0.25 * kRate);
    CHECK(rx.jumps() == 1);  // only the start's full frame
}

TEST_CASE("mtc: 29.97 non-drop and 23.976 are told apart by measurement") {
    MtcReceiverConfig config{kRate};
    for (FrameRate rate :
         {FrameRate::Fps2997, FrameRate::Fps23976, FrameRate::Fps30, FrameRate::Fps24}) {
        CAPTURE(rateName(rate));
        const LoopResult r = loop(rate, config, 5.0, 1.0);
        CHECK(r.rateMeasured);
        CHECK(r.rate == rate);
    }
    // An expected rate decides at once.
    config.expected = FrameRate::Fps2997;
    const LoopResult r = loop(FrameRate::Fps2997, config, 1.0, 1.0);
    CHECK(r.rate == FrameRate::Fps2997);
    CHECK(r.worst < 0.0006);
}

TEST_CASE("mtc: backwards, dropouts and full frames") {
    MtcReceiver rx(MtcReceiverConfig{kRate});
    const auto rate = FrameRate::Fps25;
    const double quarter = kRate / 100.0;
    // Backwards from 00:00:10:00: piece p of the sequence naming F arrives
    // when the position is F + p/4, pieces running 7..0.
    std::int64_t q = toFrames(tc(0, 0, 10, 0), rate) * 4 + 7;
    double t = 0.0;
    for (int i = 0; i < 64; ++i, --q, t += quarter) {
        const std::int64_t piece = ((q % 8) + 8) % 8;
        const Timecode label = fromFrames((q - piece) / 4, rate);
        const int nibbles[8] = {label.frames & 15,  label.frames >> 4,
                                label.seconds & 15, label.seconds >> 4,
                                label.minutes & 15, label.minutes >> 4,
                                label.hours & 15,   (label.hours >> 4) | (mtcRateCode(rate) << 1)};
        const std::uint8_t message[2] = {0xF1,
                                         static_cast<std::uint8_t>((piece << 4) | nibbles[piece])};
        const bool observed = rx.feed(t, message);
        if (rx.known()) {
            CHECK(observed);
            CHECK(rx.lastObservation().position == doctest::Approx(static_cast<double>(q) / 100.0));
        }
    }
    CHECK(rx.direction() == -1);
    CHECK(rx.locked());
    CHECK(rx.jumps() == 0);
    // Silence past the timeout: lost.
    rx.update(t + 0.2 * kRate);
    CHECK_FALSE(rx.present());
    CHECK_FALSE(rx.locked());
    CHECK_FALSE(rx.known());
    // A full frame gives the position at once and counts as a jump.
    const std::uint8_t full[10] = {0xF0, 0x7F, 0x7F, 0x01, 0x01, (1 << 5) | 2, 3, 4, 5, 0xF7};
    CHECK_FALSE(rx.feed(t, full));
    CHECK(rx.known());
    CHECK(rx.jumps() == 1);
    CHECK(rx.timecode() == tc(2, 3, 4, 5));
    CHECK(rx.lastObservation().position == doctest::Approx(2 * 3600 + 3 * 60 + 4 + 5 / 25.0));
    // A skipped piece breaks the count.
    const std::uint8_t p0[2] = {0xF1, 0x00};
    const std::uint8_t p2[2] = {0xF1, 0x20};
    rx.feed(t, p0);
    rx.feed(t + quarter, p2);
    CHECK_FALSE(rx.known());
}

namespace {
struct RtHarness {
    MtcGenerator generator{TimecodeMapping{FrameRate::Fps2997Drop, Timecode{1, 0, 0, 0}}};
    MtcReceiver receiver{MtcReceiverConfig{kRate}};
    void block(double from, const PositionMap& map, EventBuffer& out) SUPPLE_RT {
        out.clear();
        if (!generator.running()) generator.start(from, map, out);
        generator.run(from, from + 256.0, map, out);
        for (const MidiEvent& e : out.events())
            receiver.feed(e.frame, std::span(e.bytes.data(), e.size));
        receiver.update(from + 256.0);
    }
};
}  // namespace

TEST_CASE("mtc: per-block calls are real-time safe") {
    Collected c;
    RtHarness h;
    const LinearMap map(0.0, 0.0, 1.0 / kRate);
    for (double f = 0.0; f < 48000.0; f += 256.0) h.block(f, map, c.buffer);
    CHECK(h.receiver.locked());
}
