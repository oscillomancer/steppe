// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <supple/base/rng.h>
#include <supple/sync/midi_clock.h>

#include <cmath>
#include <vector>

using namespace supple;
using namespace supple::sync;

namespace {

constexpr double kRate = 48000.0;

double beatsPerFrame(double bpm) { return bpm / 60.0 / kRate; }

// Two tempos: `before` up to device frame `change`, `after` from it. Uses the
// default bisecting frameAt().
class TempoChange final : public PositionMap {
  public:
    TempoChange(double originFrame, double originBeat, double before, double change, double after)
        : originFrame_(originFrame),
          originBeat_(originBeat),
          before_(before),
          change_(change),
          after_(after) {}
    double positionAt(double frame) const SUPPLE_RT override {
        if (frame <= change_) return originBeat_ + (frame - originFrame_) * before_;
        return originBeat_ + (change_ - originFrame_) * before_ + (frame - change_) * after_;
    }

  private:
    double originFrame_, originBeat_, before_, change_, after_;
};

struct Collected {
    std::vector<MidiEvent> storage = std::vector<MidiEvent>(4096);
    EventBuffer buffer{storage};
};

void renderBlocks(ClockGenerator& g, const PositionMap& map, double from, double to, double block,
                  EventBuffer& out) SUPPLE_RT {
    for (double f = from; f < to; f += block) g.run(f, std::min(f + block, to), map, out);
}

std::vector<double> clockFrames(const EventBuffer& b) {
    std::vector<double> frames;
    for (const MidiEvent& e : b.events())
        if (e.size == 1 && e.bytes[0] == 0xF8) frames.push_back(e.frame);
    return frames;
}

}  // namespace

TEST_CASE("midi clock: Start at beat 0 and clocks at exact frames") {
    Collected c;
    ClockGenerator g;
    const LinearMap map(100.0, 0.0, beatsPerFrame(120.0));
    g.start(100.0, map, c.buffer);
    renderBlocks(g, map, 100.0, 100.0 + kRate, 256.0, c.buffer);
    const auto events = c.buffer.events();
    REQUIRE(events.size() == 1 + 48);
    CHECK(events[0].bytes[0] == 0xFA);
    CHECK(events[0].frame == 100.0);
    const double perClock = kRate / (2.0 * 24.0);  // 1000 frames at 120 bpm
    for (std::size_t k = 1; k < events.size(); ++k) {
        CHECK(events[k].bytes[0] == 0xF8);
        CHECK(events[k].frame ==
              doctest::Approx(100.0 + static_cast<double>(k - 1) * perClock).epsilon(1e-12));
    }
    CHECK(g.nextClock() == 48);
}

TEST_CASE("midi clock: block size does not change the output") {
    const TempoChange map(0.0, 0.0, beatsPerFrame(97.0), 31111.5, beatsPerFrame(171.0));
    std::vector<std::vector<double>> runs;
    for (double block : {64.0, 77.0, 512.0, 4096.0, 96000.0}) {
        Collected c;
        ClockGenerator g;
        g.start(0.0, map, c.buffer);
        renderBlocks(g, map, 0.0, 96000.0, block, c.buffer);
        runs.push_back(clockFrames(c.buffer));
    }
    for (std::size_t i = 1; i < runs.size(); ++i) {
        REQUIRE(runs[i].size() == runs[0].size());
        for (std::size_t k = 0; k < runs[0].size(); ++k)
            CHECK(std::abs(runs[i][k] - runs[0][k]) < 1e-9);
    }
}

TEST_CASE("midi clock: a tempo change inside a block") {
    const double change = 10123.25;
    const TempoChange map(0.0, 0.0, beatsPerFrame(120.0), change, beatsPerFrame(90.0));
    Collected c;
    ClockGenerator g;
    g.start(0.0, map, c.buffer);
    g.run(0.0, 30000.0, map, c.buffer);
    const auto frames = clockFrames(c.buffer);
    const double beatAtChange = change * beatsPerFrame(120.0);
    for (std::size_t k = 0; k < frames.size(); ++k) {
        const double beat = static_cast<double>(k) / 24.0;
        const double expected = beat <= beatAtChange
                                    ? beat / beatsPerFrame(120.0)
                                    : change + (beat - beatAtChange) / beatsPerFrame(90.0);
        CHECK(frames[k] == doctest::Approx(expected).epsilon(1e-9));
    }
    // Every clock up to beat(30000) once.
    CHECK(frames.size() ==
          static_cast<std::size_t>(std::floor(map.positionAt(30000.0) * 24.0)) + 1);
}

TEST_CASE("midi clock: starting between sixteenths waits for the next one") {
    Collected c;
    ClockGenerator g;
    const double perFrame = beatsPerFrame(120.0);
    const LinearMap map(0.0, 1.3, perFrame);  // beat 1.3: sixteenth 5.2
    g.start(0.0, map, c.buffer);
    g.run(0.0, 4801.0, map, c.buffer);
    const auto e = c.buffer.events();
    REQUIRE(e.size() >= 3);
    CHECK(e[0].size == 3);
    CHECK(e[0].bytes[0] == 0xF2);
    CHECK(e[0].bytes[1] == 6);  // the next 16th
    CHECK(e[0].bytes[2] == 0);
    CHECK(e[1].bytes[0] == 0xFB);
    CHECK(e[1].frame == 0.0);
    CHECK(e[2].bytes[0] == 0xF8);
    CHECK(e[2].frame == doctest::Approx((1.5 - 1.3) / perFrame));
    CHECK(g.nextClock() > 36);

    // On a sixteenth: the first clock at once.
    Collected d;
    ClockGenerator h;
    const LinearMap on(0.0, 1.25, perFrame);
    h.start(0.0, on, d.buffer);
    h.run(0.0, 10.0, on, d.buffer);
    REQUIRE(d.buffer.size() == 3);
    CHECK(d.buffer.events()[0].bytes[1] == 5);
    CHECK(d.buffer.events()[2].frame == 0.0);

    // A large position splits into 7-bit halves; past the range it clamps.
    Collected far;
    ClockGenerator k;
    k.start(0.0, LinearMap(0.0, 1000.0, perFrame), far.buffer);
    CHECK(far.buffer.events()[0].bytes[1] == (4000 & 0x7F));
    CHECK(far.buffer.events()[0].bytes[2] == (4000 >> 7));
    Collected huge;
    ClockGenerator l;
    l.start(0.0, LinearMap(0.0, 5000.0, perFrame), huge.buffer);
    CHECK(huge.buffer.events()[0].bytes[1] == 0x7F);
    CHECK(huge.buffer.events()[0].bytes[2] == 0x7F);
    CHECK(l.nextClock() == 20000 * 6);
}

TEST_CASE("midi clock: before beat 0, Start and the first clock at beat 0") {
    Collected c;
    ClockGenerator g;
    const LinearMap map(0.0, -1.0, beatsPerFrame(120.0));
    g.start(0.0, map, c.buffer);
    g.run(0.0, 30000.0, map, c.buffer);
    CHECK(c.buffer.events()[0].bytes[0] == 0xFA);
    CHECK(clockFrames(c.buffer).front() == doctest::Approx(24000.0));
}

TEST_CASE("midi clock: locate, loop wrap and stop") {
    Collected c;
    ClockGenerator g;
    const double perFrame = beatsPerFrame(120.0);
    const LinearMap first(0.0, 0.0, perFrame);
    g.start(0.0, first, c.buffer);
    g.run(0.0, 96000.0, first, c.buffer);  // beats 0..4 (beat 4 itself not yet)
    CHECK(g.nextClock() == 96);
    // Loop wrap at frame 96000 back to beat 2.1 (16th 8.4 -> 9).
    const LinearMap second(96000.0, 2.1, perFrame);
    const std::size_t before = c.buffer.size();
    g.locate(96000.0, second, c.buffer);
    g.run(96000.0, 100000.0, second, c.buffer);
    const auto e = c.buffer.events();
    CHECK(e[before].bytes[0] == 0xFC);
    CHECK(e[before + 1].bytes[0] == 0xF2);
    CHECK(e[before + 1].bytes[1] == 9);
    CHECK(e[before + 2].bytes[0] == 0xFB);
    CHECK(e[before + 3].bytes[0] == 0xF8);
    CHECK(e[before + 3].frame == doctest::Approx(96000.0 + (2.25 - 2.1) / perFrame));
    // A wrap to beat 0 restarts with Start.
    const LinearMap zero(100000.0, 0.0, perFrame);
    const std::size_t mark = c.buffer.size();
    g.locate(100000.0, zero, c.buffer);
    CHECK(c.buffer.events()[mark].bytes[0] == 0xFC);
    CHECK(c.buffer.events()[mark + 1].bytes[0] == 0xFA);
    // Stop, then nothing; a locate while stopped sends only the position.
    g.stop(100500.0, c.buffer);
    const std::size_t stopped = c.buffer.size();
    g.run(100500.0, 200000.0, zero, c.buffer);
    CHECK(c.buffer.size() == stopped);
    g.stop(100600.0, c.buffer);
    CHECK(c.buffer.size() == stopped);
    g.locate(100600.0, LinearMap(100600.0, 3.0, perFrame), c.buffer);
    REQUIRE(c.buffer.size() == stopped + 1);
    CHECK(c.buffer.events()[stopped].bytes[0] == 0xF2);
    CHECK(c.buffer.events()[stopped].bytes[1] == 12);
}

TEST_CASE("midi clock: a full buffer defers the rest") {
    std::vector<MidiEvent> small(10);
    EventBuffer b(small);
    ClockGenerator g;
    const LinearMap map(0.0, 0.0, beatsPerFrame(120.0));
    g.start(0.0, map, b);
    g.run(0.0, 48000.0, map, b);
    CHECK(b.overflowed());
    CHECK(b.size() == 10);
    CHECK(g.nextClock() == 9);
    b.clear();
    g.run(48000.0, 48001.0, map, b);
    // The deferred clocks come first, at the block start.
    CHECK(b.size() == 10);
    CHECK(b.events()[0].frame == 48000.0);
    CHECK(ClockGenerator::maxEventsPerBlock(48000.0, beatsPerFrame(120.0)) >= 49 + 3);
}

TEST_CASE("midi clock: the receiver follows the generator through jitter") {
    Collected c;
    ClockGenerator g;
    const double bpm = 123.4;
    const LinearMap map(0.0, 0.0, beatsPerFrame(bpm));
    g.start(0.0, map, c.buffer);
    ClockReceiver rx(ClockReceiverConfig{kRate});
    Rng rng(7);
    const double jitter = 0.001 * kRate;  // +-1 ms, uniform
    double lockedAt = -1.0;
    double worstBpm = 0.0;
    double worstPosition = 0.0;
    for (double f = 0.0; f < 20.0 * kRate; f += 512.0) {
        c.buffer.clear();
        if (f == 0.0) g.start(0.0, map, c.buffer);
        g.run(f, f + 512.0, map, c.buffer);
        for (const MidiEvent& e : c.buffer.events()) {
            const double at = e.frame + (e.bytes[0] == 0xF8 ? rng.uniform(-jitter, jitter) : 0.0);
            const bool observed = rx.feed(at, std::span(e.bytes.data(), e.size));
            if (observed && rx.locked()) {
                if (lockedAt < 0) lockedAt = at;
                if (at > lockedAt + 3.0 * kRate) {
                    worstBpm = std::max(worstBpm, std::abs(rx.bpm() - bpm));
                    const Observation o = rx.lastObservation();
                    worstPosition =
                        std::max(worstPosition, std::abs(o.position - map.positionAt(o.frame)));
                }
            }
        }
        rx.update(f + 512.0);
    }
    CHECK(rx.running());
    CHECK(rx.present());
    MESSAGE("locked after ", lockedAt / kRate, " s; worst tempo error ", worstBpm,
            " bpm; worst position error ", worstPosition * 60 / bpm * 1000, " ms");
    REQUIRE(lockedAt > 0.0);
    CHECK(lockedAt < 1.0 * kRate);             // locked within a second
    CHECK(worstBpm < 0.0006 * bpm);            // tempo within 0.06%
    CHECK(worstPosition < 0.0006 * bpm / 60);  // filtered times within 0.6 ms
}

TEST_CASE("midi clock: a dropout while running keeps the position") {
    for (double gapSeconds : {0.1, 0.6}) {
        CAPTURE(gapSeconds);
        Collected c;
        ClockGenerator g;
        const double bpm = 128.0;
        const LinearMap map(0.0, 0.0, beatsPerFrame(bpm));
        ClockReceiver rx(ClockReceiverConfig{kRate});
        Rng rng(4);
        bool lost = false;
        double worst = 0.0;
        double relockedAt = -1.0;
        const double dropFrom = 3.0 * kRate;
        const double dropTo = dropFrom + gapSeconds * kRate;
        for (double f = 0.0; f < 8.0 * kRate; f += 512.0) {
            c.buffer.clear();
            if (f == 0.0) g.start(0.0, map, c.buffer);
            g.run(f, f + 512.0, map, c.buffer);
            for (const MidiEvent& e : c.buffer.events()) {
                if (e.frame >= dropFrom && e.frame < dropTo) continue;
                const double at = e.frame + rng.uniform(-48.0, 48.0);
                if (rx.feed(at, std::span(e.bytes.data(), e.size)) && at > dropTo) {
                    const Observation o = rx.lastObservation();
                    worst = std::max(worst, std::abs(o.position - map.positionAt(o.frame)));
                    if (relockedAt < 0 && rx.locked()) relockedAt = at;
                }
            }
            rx.update(f + 512.0);
            if (f > dropFrom && f < dropTo && !rx.present()) lost = true;
        }
        CHECK(lost == (gapSeconds > 0.25));
        CHECK(worst < 0.01);  // under a quarter of a clock
        REQUIRE(relockedAt > 0);
        CHECK(relockedAt - dropTo < 1.0 * kRate);
    }
}

TEST_CASE("midi clock: tempo steps, Song Position, Stop and dropouts") {
    ClockReceiver rx(ClockReceiverConfig{kRate});
    const std::uint8_t clock[] = {0xF8};
    const std::uint8_t start[] = {0xFA};
    const std::uint8_t stop[] = {0xFC};
    const std::uint8_t cont[] = {0xFB};
    const std::uint8_t spp[] = {0xF2, 0x10, 0x00};  // 16 sixteenths: beat 4
    double t = 0.0;
    CHECK_FALSE(rx.feed(t, start));
    CHECK(rx.running());
    CHECK(rx.jumps() == 1);
    double period = kRate * 60.0 / (120.0 * 24.0);
    for (int i = 0; i < 24 * 8; ++i, t += period) rx.feed(t, clock);
    CHECK(rx.bpm() == doctest::Approx(120.0).epsilon(1e-6));
    CHECK(rx.locked());
    CHECK(rx.lastObservation().position == doctest::Approx(8.0 - 1.0 / 24.0));
    // A step to 140 bpm is followed within two seconds.
    period = kRate * 60.0 / (140.0 * 24.0);
    for (int i = 0; i < 24 * 5; ++i, t += period) rx.feed(t, clock);
    CHECK(rx.bpm() == doctest::Approx(140.0).epsilon(0.005));
    // Stop: the position is where a Continue resumes.
    rx.feed(t, stop);
    CHECK_FALSE(rx.running());
    CHECK(rx.positionAt(t) == doctest::Approx(13.0));
    // Song Position then Continue: the next clock is beat 4.
    rx.feed(t, spp);
    rx.feed(t, cont);
    CHECK(rx.jumps() == 3);
    CHECK(rx.feed(t, clock));
    CHECK(rx.lastObservation().position == 4.0);
    t += period;
    CHECK(rx.positionAt(t - period / 2) == doctest::Approx(4.0 + 0.5 / 24.0).epsilon(0.01));
    // Nothing for half a second: lost, but still running.
    rx.update(t + 0.5 * kRate);
    CHECK_FALSE(rx.present());
    CHECK_FALSE(rx.locked());
    CHECK(rx.running());
    // Other messages are ignored.
    const std::uint8_t note[] = {0x90, 60, 100};
    CHECK_FALSE(rx.feed(t, note));
}

namespace {
struct RtHarness {
    ClockGenerator generator;
    ClockReceiver receiver{ClockReceiverConfig{kRate}};
    void block(double from, const PositionMap& map, EventBuffer& out) SUPPLE_RT {
        out.clear();
        if (!generator.running()) generator.start(from, map, out);
        generator.run(from, from + 256.0, map, out);
        for (const MidiEvent& e : out.events())
            receiver.feed(e.frame, std::span(e.bytes.data(), e.size));
        receiver.update(from + 256.0);
        if (from > 20000.0) generator.locate(from, map, out);
    }
};
}  // namespace

TEST_CASE("midi clock: per-block calls are real-time safe") {
    Collected c;
    RtHarness h;
    const LinearMap map(0.0, 0.0, beatsPerFrame(140.0));
    for (double f = 0.0; f < 24000.0; f += 256.0) h.block(f, map, c.buffer);
    CHECK(h.receiver.running());
}
