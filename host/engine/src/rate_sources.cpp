// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "plan.h"
#include "supple/dsp/dsp.h"
#include "supple/model/fade_shape.h"

namespace supple::engine::detail {
namespace {
struct Source {
    const Clip& clip;
    const PlayrateTimeline& timeline;
    const float* fetch(std::size_t channel, std::int64_t first,
                       std::span<float> scratch) const SUPPLE_RT {
        const auto& data = clip.data->channels[channel];
        const auto size = static_cast<std::int64_t>(data.size());
        // A frame of the source, or -1 for silence: outside the item's span
        // the converter reads nothing (its trimmed media is not on the
        // timeline). A loop wrap keeps the converter's history (ADR-0045):
        // past the seam it reads the loop end's material, silent here when
        // the item had ended there, as before the item on the first pass.
        auto frameAt = [&](std::int64_t frame) -> std::int64_t {
            const double project = double(frame - clip.offset + clip.start - clip.deviceShift);
            frame += static_cast<std::int64_t>(std::llround(timeline.wrapped(project) - project));
            const auto k = frame - clip.offset;  // within the item
            if (k < 0 || k >= clip.length) return -1;
            if (clip.loop && size > 0) frame = (frame % size + size) % size;
            return frame;
        };
        const auto begin = frameAt(first);
        const auto last = frameAt(first + static_cast<std::int64_t>(scratch.size()) - 1);
        if (begin >= 0 && last < size &&
            last == begin + static_cast<std::int64_t>(scratch.size()) - 1)
            return data.data() + begin;
        for (std::size_t i = 0; i < scratch.size(); ++i) {
            const auto frame = frameAt(first + static_cast<std::int64_t>(i));
            scratch[i] = frame >= 0 && frame < size ? data[static_cast<std::size_t>(frame)] : 0;
        }
        return scratch.data();
    }
    static const float* read(const void* ctx, std::size_t channel, std::int64_t first,
                             std::span<float> scratch) noexcept {
        return static_cast<const Source*>(ctx)->fetch(channel, first, scratch);
    }
    RateSource reader() const { return {this, read, clip.data->channelCount()}; }
};

std::int64_t quantumAt(const Plan& p, std::int64_t pos) SUPPLE_RT {
    const auto offset = pos - p.rateOrigin;
    auto q = offset / kRateQuantum;
    if (offset < 0 && offset % kRateQuantum != 0) --q;
    return p.rateOrigin + q * kRateQuantum;
}

double sourcePosition(const Clip& clip, const PlayrateTimeline& timeline,
                      std::int64_t signalDevice) SUPPLE_RT {
    return timeline.absolutePhase(signalDevice) -
           (double(clip.start - clip.deviceShift) + timeline.origin()) + double(clip.offset);
}

bool processQuantum(RateClipState& state, const Clip& clip, const PlayrateTimeline& timeline,
                    std::int64_t q) SUPPLE_RT {
    Source source{clip, timeline};
    bool ok;
    if (state.preserve) {
        const double end =
            sourcePosition(clip, timeline, q + kRateQuantum + state.stretch->lookahead()) +
            state.stretch->analysisLookahead();
        ok = state.primed && state.stretch->process(source.reader(), end, state.pointers);
    } else {
        std::array<double, kRateQuantum + 1> positions;
        for (std::size_t j = 0; j < positions.size(); ++j)
            positions[j] = sourcePosition(clip, timeline, q + static_cast<std::int64_t>(j));
        ok = state.tape.process(source.reader(), positions, state.pointers);
    }
    state.failed |= !ok;
    if (!ok)
        for (auto& channel : state.output) channel.fill(0);
    state.quantum = q;
    return ok;
}

bool primeClip(RateClipState& state, const Clip& clip, PlayrateTimeline& timeline,
               std::int64_t device) {
    state.quantum = INT64_MIN;
    if (!state.preserve) return true;
    Source source{clip, timeline};
    const int latency = state.stretch->lookahead();
    const int quanta = (latency + kRateQuantum - 1) / kRateQuantum;
    const int extra = quanta * kRateQuantum - latency;
    const double start = sourcePosition(clip, timeline, device - extra);
    state.stretch->seek(source.reader(), start + extra, 1.);
    for (int q = 1; q <= quanta; ++q) {
        const auto at = device - quanta * kRateQuantum + q * kRateQuantum + latency;
        const double end = sourcePosition(clip, timeline, at) + state.stretch->analysisLookahead();
        if (!state.stretch->primeTo(source.reader(), end)) return false;
    }
    state.primed = true;
    return true;
}
}  // namespace

void prepareRateWindow(Plan& p, std::int64_t pos, std::uint32_t frames, int liveClass,
                       const LoopRange* loop) SUPPLE_RT {
    if (!p.rateEnabled) return;
    for (int c = 0; c < 2; ++c) {
        if (liveClass >= 0 && liveClass != c) continue;
        auto& timeline = *p.timelines[static_cast<std::size_t>(c)];
        if (loop) timeline.loop(double(loop->start), double(loop->end), loop->active());
        const auto end =
            pos + frames + p.rateDeviceMargin + static_cast<std::int64_t>(p.rate * .15) + 128;
        timeline.ensure(end);
        if (p.previousTimelines[static_cast<std::size_t>(c)])
            p.previousTimelines[static_cast<std::size_t>(c)]->ensure(end);
    }
}

Status prepareRateSources(Plan& p, std::int64_t device, double timeline, const LoopRange* loop) {
    if (!p.rateEnabled) return {};
    p.rateOrigin = device;
    p.rateControlsChanged = false;
    p.rateSwitchDevice = p.rateSafeDevice = INT64_MIN;
    for (auto& clock : p.previousTimelines) clock.reset();
    for (auto& node : p.nodes)
        for (auto& clip : node.clips) clip.rateState->previous.reset();
    for (auto& clock : p.timelines) clock->reset(device, timeline);
    prepareRateWindow(p, device, p.block, -1, loop);
    return primeRateSources(p, device);
}

Status primeRateSources(Plan& p, std::int64_t device) {
    dsp::DenormalGuard denormals;
    if (!p.rateEnabled) return {};
    prepareRateWindow(p, device, p.block, -1);
    for (std::size_t c = 0; c < 2; ++c) p.rateControl[c] = p.timelines[c]->snapshot(device);
    const auto commonStart = quantumAt(p, device - p.rateSourceBehind);
    for (auto& node : p.nodes) {
        auto& clock = *p.timelines[node.live ? 1 : 0];
        for (auto& clip : node.clips) {
            auto& state = *clip.rateState;
            const auto ahead =
                clip.takeChain >= 0
                    ? node.takeChains[static_cast<std::size_t>(clip.takeChain)].latency
                    : 0;
            const auto first = quantumAt(p, device + ahead - clip.deviceShift);
            const auto start = p.rateControlsChanged ? p.rateSwitchDevice : commonStart;
            if (!primeClip(state, clip, clock, start))
                return Error("playrate.trajectory",
                             "source priming exceeds the supported trajectory", {},
                             "restore a 0.25..4 rate envelope and seek again");
            if (!p.rateControlsChanged && state.preserve)
                for (auto q = start; q < first; q += kRateQuantum)
                    if (!processQuantum(state, clip, clock, q))
                        return Error("playrate.trajectory",
                                     "source preroll exceeds the supported trajectory", {},
                                     "seek again");
            if (state.previous) {
                auto& old = *p.previousTimelines[node.live ? 1 : 0];
                if (!primeClip(*state.previous, clip, old, commonStart))
                    return Error("playrate.trajectory", "previous source priming failed", {},
                                 "seek again");
                if (state.previous->preserve)
                    for (auto q = commonStart; q < first; q += kRateQuantum)
                        if (!processQuantum(*state.previous, clip, old, q))
                            return Error("playrate.trajectory", "previous source preroll failed",
                                         {}, "seek again");
            }
        }
    }
    p.ratePrepared = true;
    return {};
}

std::uint32_t rateUntil(Plan& p, std::int64_t pos, std::uint32_t frames, const LoopRange& loop,
                        int liveClass) SUPPLE_RT {
    if (!p.rateEnabled) return loop.until(pos, frames);
    const auto offset = static_cast<std::uint64_t>(pos - p.rateOrigin) & 63;
    frames = std::min(frames, static_cast<std::uint32_t>(64 - offset));
    prepareRateWindow(p, pos, frames, liveClass, &loop);
    return p.timelines[liveClass == 1 ? 1 : 0]->untilWrap(pos, frames);
}
bool rateWrapped(const Plan& p, std::int64_t from, std::int64_t to, const LoopRange& loop,
                 bool live) SUPPLE_RT {
    if (!p.rateEnabled) return loop.next(from, to) != to;
    const auto& clock = *p.timelines[live ? 1 : 0];
    return clock.cycle(clock.phase(from)) != clock.cycle(clock.phase(to));
}
RateAdoption rateAdoption(const Plan& p, const Plan* previous, std::int64_t stream,
                          std::uint64_t epoch, bool playing) SUPPLE_RT {
    if (p.handEpoch > epoch) return RateAdoption::Wait;
    if (!p.ratePrepared || p.handEpoch != epoch) return RateAdoption::Reseek;
    if (p.rateSeekSequence != 0 || !playing) return RateAdoption::Ready;
    // The consumed clock can name an older unity plan than the renderer runs
    // (unity plans publish no trajectory of their own); between unity plans
    // the identity trajectory is the same, so either is the predecessor.
    const bool predecessor = p.rateFromGeneration == (previous ? previous->generation : 0) ||
                             (p.rateFromUnity && (!previous || !previous->rateEnabled));
    if (stream > p.handAt || !predecessor) return RateAdoption::Reseek;
    return stream == p.handAt ? RateAdoption::Ready : RateAdoption::Wait;
}

Status configureRateTransition(Plan& p, Plan* previous, PlayrateTimeline& old,
                               std::int64_t device) {
    if (previous && previous->rateEnabled &&
        (p.masterPlayrate.preservePitch || previous->masterPlayrate.preservePitch)) {
        // Moving an existing stretch source in compensated device time needs
        // its past converter state, not just past clock samples. Independent
        // priming would shift its phase relative to unchanged linked sources.
        for (const auto& node : p.nodes)
            for (const auto& clip : node.clips)
                for (const auto& oldNode : previous->nodes) {
                    if (oldNode.id != node.id) continue;
                    for (const auto& oldClip : oldNode.clips) {
                        if (oldClip.item != clip.item || oldClip.take != clip.take ||
                            oldClip.data != clip.data)
                            continue;
                        const auto before =
                            (oldClip.takeChain >= 0
                                 ? oldNode.takeChains[static_cast<std::size_t>(oldClip.takeChain)]
                                       .latency
                                 : 0) -
                            oldClip.deviceShift;
                        const auto after =
                            (clip.takeChain >= 0
                                 ? node.takeChains[static_cast<std::size_t>(clip.takeChain)].latency
                                 : 0) -
                            clip.deviceShift;
                        if (before != after)
                            return Error(
                                "playrate.history",
                                "a compensated stretch source needs fresh converter history", {},
                                "prepare a fresh seek at the consumed position");
                    }
                    break;
                }
    }
    const bool changed = !previous || !previous->rateEnabled ||
                         p.masterPlayrate.rate != previous->requestedScalar ||
                         p.masterPlayrate.preservePitch != previous->masterPlayrate.preservePitch ||
                         p.timelines[0]->points() != old.points();
    p.rateControlsChanged = changed;
    const auto ahead = std::max(p.rateSourceAhead, previous ? previous->rateSourceAhead : 0);
    p.rateSwitchDevice = changed ? quantumAt(p, device + ahead + 63) : previous->rateSwitchDevice;
    p.rateSafeDevice = changed ? p.rateSwitchDevice + p.smoothFrames + p.rateSourceBehind + 64
                               : previous->rateSafeDevice;
    const auto wait = changed ? static_cast<std::uint32_t>(p.rateSwitchDevice - device) : 0;
    const auto first = device - p.rateDeviceMargin - 64;
    const auto last = device + wait + p.rateDeviceMargin + static_cast<std::int64_t>(p.rate * .15) +
                      p.block + 128;
    // Check before advancing the old ring: a large latency jump must not evict
    // the trajectory still used by playback while its replacement is prepared.
    if (last - first >= static_cast<std::int64_t>(old.capacity()) ||
        device - first >
            static_cast<std::int64_t>(std::min(old.capacity(), p.timelines[0]->capacity()) / 2) ||
        !old.hasHistory(first, device))
        return Error("playrate.history", "the changed graph needs unavailable clock history", {},
                     "prepare a fresh seek at the consumed position");
    old.ensure(last);
    p.scalarHistory.clear();
    if (previous)
        for (const auto& edit : previous->scalarHistory)
            if (edit.device > device) p.scalarHistory.push_back(edit);
    // Leaving 1.0 for a scalar rate in the same mode, from the unity bypass or a
    // preserve-pitch plan compiled at 1.0 (Engine::loadScalarRate), is a knob
    // move: glide as live knob moves do, not in 10 ms. The rate holds 1.0
    // through the converter crossfade, so both sources agree while both play.
    const bool knob = previous && p.timelines[0]->points().empty() && old.points().empty() &&
                      (!previous->rateEnabled || previous->requestedScalar == 1) &&
                      p.masterPlayrate.preservePitch == previous->masterPlayrate.preservePitch;
    const auto ramp = knob ? scalarRampFrames(p.rate) : p.smoothFrames;
    const auto smoothing = knob ? scalarSmoothingFrames(p.rate) : 0u;
    for (std::size_t c = 0; c < 2; ++c) {
        p.timelines[c]->adopt(old, device, ramp, wait, changed, smoothing);
        const auto& prior = old;
        p.previousTimelines[c] = std::make_unique<PlayrateTimeline>(
            old.capacity(), prior.snapshot(device).clock.target, prior.points());
        p.previousTimelines[c]->adopt(prior, device, 0, 0, false);
        for (auto edit : p.scalarHistory) {
            edit.sequence = 0;
            p.previousTimelines[c]->scalarEdits.push(edit);
            if (!changed) p.timelines[c]->scalarEdits.push(edit);
        }
    }
    for (auto& node : p.nodes)
        for (auto& clip : node.clips) {
            // Prepared fallback for newly added/replaced sources. Existing sources
            // transfer their running converter at the executor's adoption boundary.
            static const auto table = std::make_shared<const TapeTable>();
            const bool mode = previous && previous->masterPlayrate.preservePitch &&
                              (previous->masterPlayrate.rate != 1 || !old.points().empty());
            clip.rateState->previous =
                std::make_unique<RateClipState>(table, clip.data->channelCount(), p.rate, mode);
        }
    if (changed) p.scalarHistory.clear();
    return primeRateSources(p, device);
}

void carryRateSources(Plan& from, Plan& to, int liveClass) SUPPLE_RT {
    if (!from.rateEnabled || !to.rateEnabled || to.rateSeekSequence != 0) return;
    for (auto& oldNode : from.nodes) {
        if (liveClass >= 0 && oldNode.live != (liveClass == 1)) continue;
        for (auto& node : to.nodes) {
            if (node.id != oldNode.id) continue;
            for (auto& old : oldNode.clips)
                for (auto& clip : node.clips) {
                    if (old.item != clip.item || old.take != clip.take || old.data != clip.data ||
                        old.offset != clip.offset ||
                        old.start - old.deviceShift != clip.start - clip.deviceShift ||
                        old.loop != clip.loop)
                        continue;
                    const auto oldAhead =
                        old.takeChain >= 0
                            ? oldNode.takeChains[static_cast<std::size_t>(old.takeChain)].latency
                            : 0;
                    const auto newAhead =
                        clip.takeChain >= 0
                            ? node.takeChains[static_cast<std::size_t>(clip.takeChain)].latency
                            : 0;
                    if (oldAhead - old.deviceShift != newAhead - clip.deviceShift) continue;
                    if (!to.rateControlsChanged)
                        clip.rateState.swap(old.rateState);
                    else if (clip.rateState->previous) {
                        clip.rateState->previous.swap(old.rateState);
                        // Retire the completed fade through the old plan. These
                        // swaps never destroy a converter on an audio executor.
                        if (clip.rateState->previous->previous)
                            clip.rateState->previous->previous.swap(old.rateState->previous);
                    }
                    break;
                }
            break;
        }
    }
}

void playRateClip(Plan& p, Clip& clip, Stereo& into, std::int64_t pos, std::uint32_t frames,
                  std::int64_t ahead, bool live) SUPPLE_RT {
    auto& selected = *clip.rateState;
    auto& clock = *p.timelines[live ? 1 : 0];
    const auto channels = clip.data->channelCount();
    if (channels == 0) return;
    for (std::uint32_t i = 0; i < frames; ++i) {
        const auto device = pos + i + ahead - clip.deviceShift;
        const auto q = quantumAt(p, device);
        const bool before = selected.previous && device < p.rateSwitchDevice;
        auto& state = before ? *selected.previous : selected;
        auto& trajectory = before ? *p.previousTimelines[live ? 1 : 0] : clock;
        if (q != state.quantum) processQuantum(state, clip, trajectory, q);
        const double k = clock.absolutePosition(device) -
                         (double(clip.start - clip.deviceShift) + clock.origin());
        if (k < 0 || k >= double(clip.length)) continue;
        const auto index = static_cast<std::size_t>(device - q);
        float left = state.output[0][index], right = state.output[channels > 1 ? 1 : 0][index];
        if (!before && selected.previous && device < p.rateSwitchDevice + p.smoothFrames) {
            auto& old = *selected.previous;
            if (q != old.quantum) processQuantum(old, clip, *p.previousTimelines[live ? 1 : 0], q);
            const float mix =
                static_cast<float>(double(device - p.rateSwitchDevice) / p.smoothFrames);
            left = old.output[0][index] + mix * (left - old.output[0][index]);
            right = old.output[channels > 1 ? 1 : 0][index] +
                    mix * (right - old.output[channels > 1 ? 1 : 0][index]);
        }
        switch (clip.mode) {
            case model::ChannelMode::Normal:
                break;
            case model::ChannelMode::ReverseStereo:
                std::swap(left, right);
                break;
            case model::ChannelMode::MonoMix:
                left = right = .5f * (left + right);
                break;
            case model::ChannelMode::Left:
                right = left;
                break;
            case model::ChannelMode::Right:
                left = right;
                break;
        }
        double fade = 1.;
        if (clip.fadeIn > 0 && k < double(clip.fadeIn))
            fade *= model::fadeGain(clip.fadeInShape, clip.fadeInCurve, k / double(clip.fadeIn));
        if (clip.fadeOut > 0 && k >= double(clip.length - clip.fadeOut))
            fade *= model::fadeGain(clip.fadeOutShape, clip.fadeOutCurve,
                                    (double(clip.length) - k) / double(clip.fadeOut));
        into.l[i] += static_cast<float>(double(left) * clip.gainL * fade);
        into.r[i] += static_cast<float>(double(right) * clip.gainR * fade);
    }
}

}  // namespace supple::engine::detail
