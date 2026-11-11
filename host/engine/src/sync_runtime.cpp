// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "sync_runtime.h"

#include <algorithm>
#include <cmath>

namespace supple::engine {

std::string_view syncSourceName(SyncSource source) {
    switch (source) {
        case SyncSource::Off:
            return "off";
        case SyncSource::MidiClock:
            return "midi_clock";
        case SyncSource::Mtc:
            return "mtc";
        case SyncSource::Ltc:
            return "ltc";
    }
    return "off";
}

namespace detail {

SyncRuntime::SyncRuntime(const SyncConfig& config, double sampleRate)
    : config_(config),
      sampleRate_(sampleRate),
      follow_(config.followTempo),
      offset_(config.offsetSeconds),
      freewheel_(config.freewheelSeconds),
      clock_(sync::ClockReceiverConfig{sampleRate}),
      mtc_([&] {
          sync::MtcReceiverConfig c;
          c.sampleRate = sampleRate;
          c.expected = config.timecode.rate;  // the project's rate decides 24/23.976, 30/29.97
          return c;
      }()),
      tracker_([&] {
          sync::ChaseConfig c;
          c.sampleRate = sampleRate;
          return c;
      }()) {
    status_.source = config.source;
}

Result<std::unique_ptr<SyncRuntime>> SyncRuntime::create(const SyncConfig& config,
                                                         double sampleRate, std::uint32_t id) {
    std::unique_ptr<SyncRuntime> rt(new SyncRuntime(config, sampleRate));
    rt->status_.runtime = id;
    if (config.source == SyncSource::Ltc) {
        sync::LtcDecoderConfig d;
        d.sampleRate = sampleRate;
        d.expected = config.timecode.rate;
        auto decoder = sync::LtcDecoder::create(d);
        if (!decoder) return std::move(decoder).error();
        rt->ltcIn_.emplace(std::move(*decoder));
    }
    if (config.ltcOutput >= 0) {
        sync::LtcEncoderConfig e;
        e.sampleRate = sampleRate;
        e.mapping = config.timecode;
        e.levelDb = config.ltcLevelDb;
        auto encoder = sync::LtcEncoder::create(e);
        if (!encoder) return std::move(encoder).error();
        rt->ltcOut_.emplace(std::move(*encoder));
        // Two spans a cycle, for plan latencies of many cycles (a second at
        // 32-frame blocks, at least).
        rt->spans_.resize(4096);
    }
    return rt;
}

void SyncRuntime::adjust(const SyncConfig& config) {
    follow_.store(config.followTempo, std::memory_order_relaxed);
    offset_.store(config.offsetSeconds, std::memory_order_relaxed);
    freewheel_.store(config.freewheelSeconds, std::memory_order_relaxed);
}

double SyncRuntime::toUnits(double seconds, const time::TimeMap* map) const SUPPLE_RT {
    const double offset = offset_.load(std::memory_order_relaxed);
    if (config_.source == SyncSource::MidiClock) return map ? map->beatAt(seconds + offset) : 0.0;
    return seconds + config_.timecode.startSeconds() + offset;
}

double SyncRuntime::toSeconds(double units, const time::TimeMap* map) const SUPPLE_RT {
    const double offset = offset_.load(std::memory_order_relaxed);
    if (config_.source == SyncSource::MidiClock)
        return (map ? map->secondsAt(std::max(units, 0.0)) : 0.0) - offset;
    return units - config_.timecode.startSeconds() - offset;
}

void SyncRuntime::feedMidi(const SyncCycle& c) SUPPLE_RT {
    for (const SyncMidiEvent& e : c.midi) {
        if (static_cast<int>(e.port) != config_.midiPort || e.size == 0) continue;
        const double frame = static_cast<double>(c.device + e.frame);
        const std::span<const std::uint8_t> bytes(e.bytes.data(), e.size);
        bool observed = false;
        std::uint32_t jumps = 0;
        if (config_.source == SyncSource::MidiClock) {
            const bool before = clock_.running();
            observed = clock_.feed(frame, bytes);
            if (!before && clock_.running()) {
                ++status_.starts;
                clocksSinceStart_ = 0;
            }
            if (before && !clock_.running()) ++status_.stops;
            if (observed) ++clocksSinceStart_;
            jumps = clock_.jumps();
        } else if (config_.source == SyncSource::Mtc) {
            observed = mtc_.feed(frame, bytes);
            jumps = mtc_.jumps();
        }
        if (jumps != receiverJumps_) {
            receiverJumps_ = jumps;
            ++status_.jumps;
            tracker_.jumped();
        }
        if (observed) {
            tracker_.observe(config_.source == SyncSource::MidiClock ? clock_.lastObservation()
                                                                     : mtc_.lastObservation());
            lastObservation_ = c.device + e.frame;
            everObserved_ = true;
        }
    }
}

void SyncRuntime::feedLtc(const SyncCycle& c) SUPPLE_RT {
    if (!ltcIn_ || config_.ltcInput < 0 ||
        static_cast<std::uint32_t>(config_.ltcInput) >= c.inputs || !c.in ||
        !c.in[config_.ltcInput])
        return;
    // Input samples were captured inputLatency frames before the callback saw them.
    const double at = static_cast<double>(c.device - c.inputLatency);
    const auto frames = ltcIn_->feed(at, std::span<const float>(c.in[config_.ltcInput], c.frames));
    if (ltcIn_->jumps() != receiverJumps_) {
        receiverJumps_ = ltcIn_->jumps();
        ++status_.jumps;
        tracker_.jumped();
    }
    for (const sync::LtcFrame& f : frames) {
        tracker_.observe(f.observation);
        lastObservation_ = static_cast<std::int64_t>(f.observation.frame);
        everObserved_ = true;
    }
}

double SyncRuntime::slope(const SyncCycle& c) const SUPPLE_RT {
    if (c.frames == 0) return 1.0 / sampleRate_;
    const double run = c.wrapped ? (c.loopEnd - c.timelineStart) + (c.timelineEnd - c.loopStart)
                                 : c.timelineEnd - c.timelineStart;
    return run > 0.0 ? run / c.frames : 1.0 / sampleRate_;
}

void SyncRuntime::remember(const SyncCycle& c) SUPPLE_RT {
    auto push = [&](const Span& s) {
        if (s.frames <= 0) return;
        // A stopped stretch extends the stopped span before it.
        if (spanCount_ > 0) {
            Span& last = spans_[(spanHead_ + spans_.size() - 1) % spans_.size()];
            if (!s.playing && !last.playing && last.device + last.frames == s.device) {
                last.frames += s.frames;
                return;
            }
        }
        spans_[spanHead_] = s;
        spanHead_ = (spanHead_ + 1) % spans_.size();
        spanCount_ = std::min(spanCount_ + 1, spans_.size());
    };
    if (!c.playing) {
        push({c.device, c.frames, 0.0, 0.0, false});
        return;
    }
    const double k = slope(c);
    if (!c.wrapped) {
        push({c.device, c.frames, c.timelineStart, k, true});
        return;
    }
    // Split at the wrap: the frames that reached the loop's end, then the rest.
    const auto before = std::clamp<std::int64_t>(std::llround((c.loopEnd - c.timelineStart) / k), 0,
                                                 static_cast<std::int64_t>(c.frames));
    push({c.device, before, c.timelineStart, k, true});
    push({c.device + before, c.frames - before, c.loopStart, k, true});
}

void SyncRuntime::writeLtc(const SyncCycle& c) SUPPLE_RT {
    if (!ltcOut_ || config_.ltcOutput < 0 ||
        static_cast<std::uint32_t>(config_.ltcOutput) >= c.outputs || !c.out ||
        !c.out[config_.ltcOutput]) {
        status_.ltcOutput = false;
        return;
    }
    remember(c);
    float* out = c.out[config_.ltcOutput];
    status_.ltcOutput = true;
    if (!c.playing) {  // stopped: silent at once
        if (ltcOut_->running()) ltcOut_->stop();
        std::fill_n(out, c.frames, 0.0f);
        return;
    }
    // Output frame d carries the timeline processed at d - latency: the audio
    // leaving with it. Walk the spans that cover [d0 - L, d0 - L + n).
    const std::int64_t latency = std::max<std::int64_t>(c.planLatency, 0);
    const std::int64_t from = c.device - latency, to = from + c.frames;
    const double frameSeconds = sync::toSeconds(1.0, config_.timecode.rate);
    std::int64_t at = from;
    auto silence = [&](std::int64_t until) {
        if (until <= at) return;
        if (ltcOut_->running()) ltcOut_->stop();
        std::fill(out + (at - from), out + (until - from), 0.0f);
        at = until;
    };
    auto span = [&](std::size_t i) -> const Span& {  // i: 0 the oldest kept
        return spans_[(spanHead_ + spans_.size() - spanCount_ + i) % spans_.size()];
    };
    // The newest span starting at or before `from`: a few steps back.
    std::size_t first = spanCount_;
    while (first > 0 && span(first - 1).device > from) --first;
    if (first > 0) --first;
    for (std::size_t i = first; i < spanCount_ && at < to; ++i) {
        const Span& s = span(i);
        const std::int64_t end = s.device + s.frames;
        if (end <= at) continue;
        if (s.device >= to) break;
        silence(std::min(s.device, to));  // before the oldest span kept, or a gap
        const std::int64_t until = std::min(end, to);
        if (!s.playing) {
            silence(until);
            continue;
        }
        const double label = s.start + s.slope * static_cast<double>(at - s.device);
        // A start, locate or loop wrap: LTC begins at the next frame boundary.
        if (!ltcOut_->running() || std::abs(label - ltcEnd_) > frameSeconds) ltcOut_->start(label);
        const sync::LinearMap map(static_cast<double>(at + latency), label, s.slope);
        ltcOut_->render(static_cast<double>(at + latency),
                        std::span<float>(out + (at - from), static_cast<std::size_t>(until - at)),
                        map);
        ltcEnd_ = label + s.slope * static_cast<double>(until - at);
        at = until;
    }
    silence(to);
}

bool SyncRuntime::process(const SyncCycle& c) SUPPLE_RT {
    const SyncStatus before = status_;
    const std::int64_t end = c.device + c.frames;
    status_.device = c.device;
    status_.heardLatency = static_cast<double>(c.outputLatency) / sampleRate_;
    if (config_.source == SyncSource::MidiClock || config_.source == SyncSource::Mtc) feedMidi(c);
    if (config_.source == SyncSource::Ltc) feedLtc(c);
    const auto now = static_cast<double>(end);
    bool present = false, running = false, locked = false;
    switch (config_.source) {
        case SyncSource::MidiClock:
            clock_.update(now);
            present = clock_.present();
            // Running once the first clock after Start/Continue has arrived.
            running = clock_.running() && clocksSinceStart_ > 0;
            locked = clock_.locked();
            status_.bpm = clock_.bpm();
            status_.position = clock_.positionAt(now);
            status_.songPosition = clock_.positionAt(now);
            // The first clock has no tempo to count on from: the project's.
            if (clock_.running() && clock_.bpm() <= 0.0 && clocksSinceStart_ > 0 && c.timeMap) {
                const sync::Observation last = clock_.lastObservation();
                status_.position = last.position + (now - last.frame) / sampleRate_ *
                                                       c.timeMap->bpmAt(last.position) / 60.0;
            }
            break;
        case SyncSource::Mtc:
            mtc_.update(now);
            present = mtc_.present() && mtc_.known();
            running = present && mtc_.direction() > 0;
            locked = mtc_.locked();
            status_.direction = mtc_.direction();
            status_.rate = mtc_.rate();
            status_.rateKnown = mtc_.rateMeasured() || mtc_.locked();
            status_.timecode = mtc_.timecode();
            status_.position = mtc_.lastObservation().position;
            break;
        case SyncSource::Ltc:
            if (ltcIn_) {
                ltcIn_->update(now);
                present = ltcIn_->present();
                running = present && ltcIn_->direction() > 0;
                locked = ltcIn_->locked();
                status_.direction = ltcIn_->direction();
                status_.rate = ltcIn_->rate();
                status_.rateKnown = ltcIn_->rateMeasured();
                status_.timecode = ltcIn_->timecode();
                status_.position = ltcIn_->lastObservation().position;
            }
            break;
        case SyncSource::Off:
            break;
    }
    status_.lostFrames = everObserved_ ? std::max<std::int64_t>(0, end - lastObservation_) : 0;
    // Present again (a new observation): a later loss counts again.
    if (present) lossFinal_ = false;
    // A loss is final at once, or after the freewheel time.
    if (config_.source != SyncSource::Off && everObserved_ && !present && !lossFinal_ &&
        static_cast<double>(status_.lostFrames) >=
            freewheel_.load(std::memory_order_relaxed) * sampleRate_) {
        lossFinal_ = true;
        ++status_.losses;
        tracker_.reset();
    }
    status_.present = present;
    status_.running = running;
    status_.locked = locked;

    // The listener hears what was processed outputLatency frames ago.
    const double processedSpeed = c.playing && c.frames > 0 ? slope(c) * sampleRate_ : 0.0;
    // At a loop wrap, as if the cycle had played up to its end from the start.
    const double processed =
        c.wrapped ? c.timelineEnd - slope(c) * static_cast<double>(c.frames) : c.timelineStart;
    const double heard = processed - static_cast<double>(c.outputLatency) / sampleRate_ *
                                         std::max(processedSpeed, 0.0);
    const bool chasing =
        config_.source == SyncSource::Mtc || config_.source == SyncSource::Ltc ||
        (config_.source == SyncSource::MidiClock && follow_.load(std::memory_order_relaxed));
    sync::ChaseOutput chase;
    if (config_.source != SyncSource::Off && present && running) {
        double nominal = 1.0;
        if (config_.source == SyncSource::MidiClock)
            nominal = c.timeMap ? c.timeMap->bpmAt(toUnits(heard, c.timeMap)) / 60.0 : 2.0;
        chase = tracker_.update(static_cast<double>(c.device), toUnits(heard, c.timeMap), nominal);
        if (!c.playing) chase.relocate = false;  // EngineApp starts the transport itself
        if (chase.relocate && chasing) ++status_.relocates;
        status_.chaseRate = chasing && c.playing ? chase.rate : 1.0;
        status_.externalRate = chase.externalRate;
        status_.errorSeconds = chase.errorSeconds;
        status_.chase = static_cast<std::uint8_t>(chase.state);
    } else if (!(config_.source != SyncSource::Off && everObserved_ && !lossFinal_)) {
        // Not chasing, and not freewheeling either: unity.
        status_.chaseRate = 1.0;
        status_.chase = static_cast<std::uint8_t>(sync::ChaseState::Idle);
        status_.errorSeconds = 0.0;
    }
    if (everObserved_) {
        const double at = static_cast<double>(c.device + c.outputLatency);
        status_.relocateSeconds = toSeconds(tracker_.predict(at), c.timeMap);
    }
    writeLtc(c);
    ++status_.sequence;

    // What the main thread acts on.
    bool notify = before.present != status_.present || before.running != status_.running ||
                  before.locked != status_.locked || before.starts != status_.starts ||
                  before.stops != status_.stops || before.jumps != status_.jumps ||
                  before.relocates != status_.relocates || before.losses != status_.losses;
    // A new chase rate, at most every 10 ms.
    if (std::abs(status_.chaseRate - notifiedRate_) > 1e-6 &&
        end - notifiedAt_ >= static_cast<std::int64_t>(sampleRate_ / 100)) {
        notifiedRate_ = status_.chaseRate;
        notifiedAt_ = end;
        notify = true;
    }
    return notify;
}

}  // namespace detail
}  // namespace supple::engine
