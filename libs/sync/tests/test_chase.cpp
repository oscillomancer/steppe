// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <supple/base/rng.h>
#include <supple/sync/chase.h>
#include <supple/sync/midi_clock.h>

#include <cmath>
#include <deque>
#include <vector>

using namespace supple;
using namespace supple::sync;

namespace {

constexpr double kRate = 48000.0;
constexpr double kBlock = 256.0;

// The engine's side, modelled: the commanded playrate reaches the audio after
// `delaySeconds` (render-ahead and ramps) and through ADR-0046's 150 ms
// one-pole low-pass; the local position integrates the applied rate. A
// relocate lands `seekSeconds` after the request, at predict() of that
// moment plus `seekError` (a latency the engine got wrong).
struct Engine {
    double delaySeconds = 0.3;
    double lowPassSeconds = 0.15;
    double seekSeconds = 0.1;
    double seekError = 0.0;
    bool ignoreRelocates = false;
    double nominal = 1.0;  // position units per second at playrate 1
};

// The external source: position = origin + speed * t, observed every
// `period` seconds with uniform timing jitter, optionally jumping once.
struct Source {
    double origin = 3600.0;
    double speed = 1.0;
    double period = 1.0 / 100.0;  // MTC quarter frames at 25 fps
    double jitterSeconds = 0.001;
    double jumpAt = -1.0;
    double jumpBy = 0.0;
    double at(double t) const {
        return origin + speed * t + (jumpAt >= 0 && t >= jumpAt ? jumpBy : 0.0);
    }
};

struct Trace {
    std::vector<ChaseOutput> outputs;
    std::vector<double> times;
    int relocates = 0;
    double lockedAt = -1.0;
    double finalError = 0.0;
    double finalRate = 0.0;
    double worstLateError = 0.0;  // over the last quarter
    double worstLateRate = 0.0;   // |rate - source speed| over the last quarter
    int lateSignChanges = 0;
    std::uint32_t jumps = 0;
};

Trace simulate(const Source& source, const Engine& engine, double localStart, double seconds,
               ChaseConfig config = {}) {
    config.sampleRate = kRate;
    ChaseTracker tracker(config);
    Rng rng(5);
    Trace trace;
    double local = localStart;
    double applied = 1.0;
    std::deque<double> pipeline(static_cast<std::size_t>(engine.delaySeconds * kRate / kBlock),
                                1.0);
    double nextObservation = 0.0;
    double seekAt = -1.0;
    double seekTarget = 0.0;
    const double dt = kBlock / kRate;
    const double alpha = 1.0 - std::exp(-dt / engine.lowPassSeconds);
    double previousError = 0.0;
    for (double frame = 0.0; frame < seconds * kRate; frame += kBlock) {
        const double t = frame / kRate;
        while (nextObservation < t + dt) {
            const double jitter = rng.uniform(-source.jitterSeconds, source.jitterSeconds);
            tracker.observe({(nextObservation + jitter) * kRate, source.at(nextObservation)});
            nextObservation += source.period;
        }
        if (seekAt >= 0 && t >= seekAt) {
            local = seekTarget;
            seekAt = -1.0;
        }
        const ChaseOutput out = tracker.update(frame, local, engine.nominal);
        trace.outputs.push_back(out);
        trace.times.push_back(t);
        if (out.relocate) {
            ++trace.relocates;
            if (!engine.ignoreRelocates) {
                seekAt = t + engine.seekSeconds;
                seekTarget = tracker.predict((t + engine.seekSeconds) * kRate) +
                             engine.seekError * engine.nominal;
            }
        }
        if (out.state == ChaseState::Locked && trace.lockedAt < 0) trace.lockedAt = t;
        if (out.state != ChaseState::Locked && out.state != ChaseState::Chasing)
            trace.lockedAt = -1.0;
        if (t > 0.75 * seconds) {
            trace.worstLateError = std::max(trace.worstLateError, std::abs(out.errorSeconds));
            trace.worstLateRate = std::max(trace.worstLateRate, std::abs(out.rate - source.speed));
            if (previousError != 0.0 && (out.errorSeconds > 0) != (previousError > 0))
                ++trace.lateSignChanges;
            previousError = out.errorSeconds;
        }
        trace.finalError = out.errorSeconds;
        trace.finalRate = out.rate;
        // The engine: delay, low-pass, integrate.
        pipeline.push_back(out.rate);
        const double delayed = pipeline.front();
        pipeline.pop_front();
        applied += (delayed - applied) * alpha;
        local += applied * engine.nominal * dt;
    }
    trace.jumps = tracker.jumps();
    return trace;
}

}  // namespace

TEST_CASE("chase: far behind, one relocate, then locked") {
    const Trace r = simulate(Source{}, Engine{}, 3590.0, 20.0);
    MESSAGE("locked at ", r.lockedAt, " s, final error ", r.finalError * 1000, " ms, late worst ",
            r.worstLateError * 1000, " ms, late rate deviation ", r.worstLateRate);
    CHECK(r.relocates == 1);
    REQUIRE(r.lockedAt > 0.0);
    CHECK(r.lockedAt < 8.0);
    CHECK(std::abs(r.finalError) < 0.001);
    CHECK(r.worstLateError < 0.002);
    // Under 1 ms of observation jitter the rate wanders by under 0.1%.
    CHECK(r.worstLateRate < 0.001);
}

TEST_CASE("chase: a faster source is chased without relocating, and settles") {
    Source s;
    s.speed = 1.02;
    const Trace r = simulate(s, Engine{}, 3599.9, 40.0);
    MESSAGE("locked at ", r.lockedAt, " s, late worst ", r.worstLateError * 1000,
            " ms, sign changes ", r.lateSignChanges);
    CHECK(r.relocates == 0);
    REQUIRE(r.lockedAt > 0.0);
    CHECK(r.lockedAt < 20.0);
    CHECK(r.worstLateError < 0.002);
    CHECK(r.finalRate == doctest::Approx(1.02).epsilon(5e-4));
    // The correction stays inside the window: never more than 10% off.
    for (const ChaseOutput& o : r.outputs)
        if (o.state == ChaseState::Chasing || o.state == ChaseState::Locked)
            CHECK(o.rate <= 1.02 * 1.1 + 1e-9);
}

TEST_CASE("chase: stable with half a second of playrate delay") {
    Engine e;
    e.delaySeconds = 0.35;  // with the 150 ms low-pass, 0.5 s in all
    Source s;
    s.speed = 0.97;
    const Trace r = simulate(s, e, 3600.15, 60.0);
    MESSAGE("late worst ", r.worstLateError * 1000, " ms");
    CHECK(r.relocates == 0);
    CHECK(r.worstLateError < 0.002);
    // A step disturbance decays without growing oscillation: the error's
    // peaks shrink.
    double peak1 = 0.0, peak2 = 0.0;
    for (std::size_t i = 0; i < r.outputs.size(); ++i) {
        if (r.times[i] > 10.0 && r.times[i] < 20.0)
            peak1 = std::max(peak1, std::abs(r.outputs[i].errorSeconds));
        if (r.times[i] > 20.0 && r.times[i] < 30.0)
            peak2 = std::max(peak2, std::abs(r.outputs[i].errorSeconds));
    }
    CHECK(peak2 < peak1);
}

TEST_CASE("chase: relocation hysteresis") {
    // 0.2 s off: inside the chase band, no relocate.
    CHECK(simulate(Source{}, Engine{}, 3599.8, 10.0).relocates == 0);
    // 0.3 s off: one relocate after the delay.
    const Trace once = simulate(Source{}, Engine{}, 3599.7, 10.0);
    CHECK(once.relocates == 1);
    REQUIRE(!once.outputs.empty());
    std::size_t first = 0;
    while (first < once.outputs.size() && !once.outputs[first].relocate) ++first;
    // The seed (0.1 s of observations, jittered by up to 1 ms), then 0.1 s
    // above the threshold.
    CHECK(once.times[first] >= 0.19);
    // A relocate that lands 0.15 s off is chased, not repeated.
    Engine sloppy;
    sloppy.seekError = -0.15;
    const Trace chased = simulate(Source{}, sloppy, 3500.0, 20.0);
    CHECK(chased.relocates == 1);
    CHECK(chased.worstLateError < 0.002);
    // A relocate that never lands is retried each second.
    Engine deaf;
    deaf.ignoreRelocates = true;
    const Trace retried = simulate(Source{}, deaf, 3500.0, 3.55);
    CHECK(retried.relocates == 4);
    for (const ChaseOutput& o : retried.outputs)
        if (o.state == ChaseState::Relocating) CHECK(o.rate == doctest::Approx(1.0).epsilon(0.01));
}

TEST_CASE("chase: a jump in the source re-seeds and relocates") {
    Source s;
    s.jumpAt = 10.0;
    s.jumpBy = 30.0;
    const Trace r = simulate(s, Engine{}, 3600.0, 25.0);
    CHECK(r.jumps == 1);
    CHECK(r.relocates == 1);
    CHECK(r.worstLateError < 0.002);
}

TEST_CASE("chase: beats, through the MIDI clock receiver") {
    // A 130 bpm master against a 120 bpm project: playrate 130/120.
    std::vector<MidiEvent> storage(512);
    EventBuffer events(storage);
    ClockGenerator master;
    const LinearMap masterBeats(0.0, 16.0, 130.0 / 60.0 / kRate);
    ClockReceiver rx(ClockReceiverConfig{kRate});
    ChaseConfig config;
    config.sampleRate = kRate;
    ChaseTracker tracker(config);
    Rng rng(9);
    double local = 0.0;  // beats
    double applied = 1.0;
    const double nominal = 120.0 / 60.0;
    std::deque<double> pipeline(static_cast<std::size_t>(0.3 * kRate / kBlock), 1.0);
    const double dt = kBlock / kRate;
    const double alpha = 1.0 - std::exp(-dt / 0.15);
    std::uint32_t jumps = 0;
    ChaseOutput out;
    int relocates = 0;
    double worst = 0.0;
    for (double frame = 0.0; frame < 30.0 * kRate; frame += kBlock) {
        events.clear();
        if (frame == 0.0) master.start(0.0, masterBeats, events);
        master.run(frame, frame + kBlock, masterBeats, events);
        for (const MidiEvent& e : events.events()) {
            const double at = e.frame + (e.bytes[0] == 0xF8 ? rng.uniform(-48.0, 48.0) : 0.0);
            if (rx.feed(at, std::span(e.bytes.data(), e.size))) {
                if (rx.jumps() != jumps) {
                    jumps = rx.jumps();
                    tracker.jumped();
                }
                tracker.observe(rx.lastObservation());
            }
        }
        rx.update(frame + kBlock);
        out = tracker.update(frame, local, nominal);
        if (out.relocate) {
            ++relocates;
            local = tracker.predict(frame);
        }
        if (frame > 20.0 * kRate) worst = std::max(worst, std::abs(out.errorSeconds));
        pipeline.push_back(out.rate);
        applied += (pipeline.front() - applied) * alpha;
        pipeline.pop_front();
        local += applied * nominal * dt;
    }
    MESSAGE("rate ", out.rate, ", late worst ", worst * 1000, " ms");
    CHECK(relocates == 1);
    CHECK(out.state == ChaseState::Locked);
    CHECK(out.externalRate == doctest::Approx(130.0 / 120.0).epsilon(5e-4));
    CHECK(out.rate == doctest::Approx(130.0 / 120.0).epsilon(1e-3));
    CHECK(worst < 0.002);
}

TEST_CASE("chase: deterministic for a given input sequence") {
    Source s;
    s.speed = 1.013;
    const Trace a = simulate(s, Engine{}, 3599.0, 15.0);
    const Trace b = simulate(s, Engine{}, 3599.0, 15.0);
    REQUIRE(a.outputs.size() == b.outputs.size());
    bool identical = true;
    for (std::size_t i = 0; i < a.outputs.size(); ++i)
        identical = identical && a.outputs[i].rate == b.outputs[i].rate &&
                    a.outputs[i].errorSeconds == b.outputs[i].errorSeconds &&
                    a.outputs[i].relocate == b.outputs[i].relocate &&
                    a.outputs[i].state == b.outputs[i].state;
    CHECK(identical);
}

TEST_CASE("chase: idle, acquiring, limits and reset") {
    ChaseConfig config;
    config.sampleRate = kRate;
    ChaseTracker t(config);
    ChaseOutput o = t.update(0.0, 0.0, 1.0);
    CHECK(o.state == ChaseState::Idle);
    CHECK(o.rate == 1.0);
    t.observe({0.0, 10.0});
    o = t.update(0.0, 10.0, 1.0);
    CHECK(o.state == ChaseState::Acquiring);
    CHECK(t.predict(1000.0) == 10.0);
    // A source at 8x: the rate stops at maxRate.
    for (int i = 1; i <= 100; ++i) t.observe({i * 480.0, 10.0 + 8.0 * i * 0.01});
    o = t.update(100 * 480.0, 10.0 + 8.0, 1.0);
    CHECK(o.externalRate == doctest::Approx(8.0).epsilon(1e-6));
    CHECK(o.rate == 4.0);
    t.reset();
    CHECK(t.update(0.0, 0.0, 1.0).state == ChaseState::Idle);
}

namespace {
struct RtHarness {
    ChaseTracker tracker{ChaseConfig{}};
    ChaseOutput block(double frame, double local) SUPPLE_RT {
        tracker.observe({frame, frame / 48000.0});
        return tracker.update(frame, local, 1.0);
    }
};
}  // namespace

TEST_CASE("chase: per-block calls are real-time safe") {
    RtHarness h;
    ChaseOutput o;
    for (double f = 0.0; f < 96000.0; f += 256.0) o = h.block(f, f / 48000.0);
    CHECK(o.state == ChaseState::Locked);
}
