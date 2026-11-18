// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A file audition in the device callback (ADR-0094, engine.md "File
// audition"): decoded audio at the device's rate, added to outputs 1/2 after
// the project's output, outside the plan. Renders, recordings and meters
// never see it.
#include <algorithm>
#include <cmath>

#include "supple/engine/realtime.h"

namespace supple::engine {

std::uint64_t Engine::audition(std::shared_ptr<const AuditionVoice> voice) {
    auto request = std::make_unique<AuditionRequest>();
    if (voice && voice->frames() > 0) request->voice = std::move(voice);
    request->serial = ++auditionSerial_;
    const AuditionRequest* published = request.get();
    auditionOwned_.push_back(std::move(request));
    auditionNext_.store(published, std::memory_order_release);
    reclaimAuditions();
    return published->voice ? published->serial : 0;
}

AuditionState Engine::auditionState() const {
    AuditionState s;
    s.serial = auditionPlaying_.load(std::memory_order_acquire);
    s.position = auditionPosition_.load(std::memory_order_relaxed);
    s.playing = s.serial != 0;
    s.taken = auditionTaken_.load(std::memory_order_acquire);
    return s;
}

void Engine::reclaimAuditions() {
    // Every request below the smallest serial the callback may still read,
    // except the newest (published, perhaps not yet seen).
    const std::uint64_t keep = auditionKeep_.load(std::memory_order_acquire);
    while (auditionOwned_.size() > 1 && auditionOwned_.front()->serial < keep)
        auditionOwned_.pop_front();
}

void Engine::resetAuditions() {
    // No callback runs (before start, after stop): what was published goes.
    auditionNext_.store(nullptr, std::memory_order_release);
    auditionSeen_ = nullptr;
    auditionPlay_ = {};
    auditionFade_ = {};
    auditionFadeLeft_ = 0;
    auditionKeep_.store(0, std::memory_order_release);
    auditionPlaying_.store(0, std::memory_order_release);
    auditionTaken_.store(0, std::memory_order_release);
    auditionPosition_.store(0, std::memory_order_relaxed);
    auditionOwned_.clear();
}

void Engine::mixAudition(float* const* out, std::uint32_t frames) SUPPLE_RT {
    if (const AuditionRequest* next = auditionNext_.load(std::memory_order_acquire);
        next != auditionSeen_) {
        auditionSeen_ = next;
        // What plays fades out (a fade still running is cut: it is older).
        if (auditionPlay_.voice) {
            auditionFade_ = auditionPlay_;
            const double rate = deviceRate_.load(std::memory_order_relaxed);
            auditionFadeLeft_ = std::max<std::uint32_t>(
                1, static_cast<std::uint32_t>(std::lround(rate * kAuditionFadeSeconds)));
            auditionFadeFrames_ = auditionFadeLeft_;
        }
        auditionPlay_ = {};
        if (next && next->voice) auditionPlay_ = {next->voice.get(), 0, next->serial};
    }
    // The engine has fewer than two outputs only if it never started
    // (Engine::start refuses); a stopping engine is silent from here.
    const bool audible = deviceOutputs_ >= 2 && !shutdownRequested_.load(std::memory_order_acquire);
    auto add = [&](AuditionPlay& p, std::uint32_t n, auto gain) {
        const AuditionVoice& v = *p.voice;
        const std::vector<float>& r = v.right.empty() ? v.left : v.right;
        for (std::uint32_t i = 0; i < n; ++i) {
            const float g = gain(i);
            out[0][i] += g * v.left[p.position + i];
            out[1][i] += g * r[p.position + i];
        }
        p.position += n;
    };
    if (auditionFade_.voice) {
        const std::size_t left = auditionFade_.voice->frames() - auditionFade_.position;
        const auto n =
            static_cast<std::uint32_t>(std::min<std::size_t>({frames, left, auditionFadeLeft_}));
        if (audible) {
            const auto total = static_cast<float>(auditionFadeFrames_);
            const std::uint32_t done = auditionFadeFrames_ - auditionFadeLeft_;
            add(auditionFade_, n,
                [&](std::uint32_t i) { return 1.0f - static_cast<float>(done + i + 1) / total; });
        }
        auditionFadeLeft_ -= n;
        if (auditionFadeLeft_ == 0 || n == left) auditionFade_ = {};
    }
    bool ended = false;
    if (auditionPlay_.voice) {
        const std::size_t left = auditionPlay_.voice->frames() - auditionPlay_.position;
        const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(frames, left));
        if (audible)
            add(auditionPlay_, n, [](std::uint32_t) { return 1.0f; });
        else
            auditionPlay_.position += n;
        if (n == left) {
            auditionPlay_ = {};
            ended = true;
        }
    }
    auditionPosition_.store(static_cast<std::int64_t>(auditionPlay_.position),
                            std::memory_order_relaxed);
    const std::uint64_t playing = auditionPlay_.voice ? auditionPlay_.serial : 0;
    if (auditionPlaying_.load(std::memory_order_relaxed) != playing) {
        auditionPlaying_.store(playing, std::memory_order_release);
        if (ended) notifyMain();
    }
    std::uint64_t keep = auditionSeen_ ? auditionSeen_->serial : 0;
    if (auditionPlay_.voice) keep = std::min(keep, auditionPlay_.serial);
    if (auditionFade_.voice) keep = std::min(keep, auditionFade_.serial);
    auditionKeep_.store(keep, std::memory_order_release);
    auditionTaken_.store(auditionSeen_ ? auditionSeen_->serial : 0, std::memory_order_release);
}

}  // namespace supple::engine
