// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "supple/base/rt.h"
#include "supple/dsp/dsp.h"

namespace supple::analysis {
enum class Layout : std::uint8_t { Mono, Stereo, Surround50, Surround51 };
struct Signal {
    std::span<const float* const> channels;
    std::int64_t frames = 0;
    double sampleRate = 48000.0;
    Layout layout = Layout::Stereo;
};

class LoudnessMeter {
  public:
    enum class Mode { Exact, Histogram };
    void prepare(double sampleRate, Layout layout, Mode mode, double maxSeconds = 0);
    void reset() SUPPLE_RT;
    void process(std::span<const float* const> channels, int frames) SUPPLE_RT;
    double momentary() const { return momentary_; }
    double shortTerm() const { return shortTerm_; }
    double integrated() const;
    double loudnessRange() const;
    double maxMomentary() const;
    double maxShortTerm() const;
    double truePeak() const;
    double samplePeak() const;
    std::size_t blockCount() const { return blockCount_; }

  private:
    static constexpr int kBins = 15001;
    static double loudness(double energy);
    static int bin(double loudness);
    int channels_ = 0, factor_ = 1;
    int hop_ = 0, window400_ = 0, window3_ = 0;
    Mode mode_ = Mode::Histogram;
    std::array<double, 6> weights_{};
    std::array<dsp::BiquadFilter, 6> shelf_, highpass_;
    std::array<dsp::PolyphaseUpsampler, 6> oversamplers_;
    std::vector<double> ring_;
    std::vector<double> exactBlocks_, exactShort_;
    std::array<std::uint32_t, kBins> blocksHist_{}, shortHist_{};
    std::array<double, kBins> blockEnergyHist_{}, shortEnergyHist_{};
    std::size_t blockCount_ = 0, shortCount_ = 0;
    std::uint64_t samples_ = 0;
    double sum400_ = 0, sum3_ = 0;
    double momentary_ = -__builtin_huge_val(), shortTerm_ = -__builtin_huge_val();
    double maxMomentaryEnergy_ = 0, maxShortEnergy_ = 0;
    double peak_ = 0, samplePeak_ = 0;
};
}  // namespace supple::analysis
