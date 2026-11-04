// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// Deterministic pseudo-random numbers: xoshiro256** seeded with splitmix64
// (Blackman & Vigna, public-domain algorithms; implemented here from the
// published description). Identical seeds give identical streams on every
// platform for the integer and uniform functions. All functions are
// allocation-free and usable on the audio thread.
//
// Every source of randomness in Steppe (plugin noise, drift, dither, humanize,
// the optimizer) derives from an explicit seed stored in state.
#pragma once

#include <cassert>
#include <cstdint>

#include "supple/base/rt.h"

namespace supple {

class Rng {
    __extension__ using U128 = unsigned __int128;  // GCC/Clang; silences -Wpedantic

  public:
    explicit Rng(std::uint64_t seed = 0) SUPPLE_RT { reseed(seed); }

    void reseed(std::uint64_t seed) SUPPLE_RT {
        std::uint64_t x = seed;
        for (auto& word : s_) word = splitmix64(x);
    }

    // 64 uniformly distributed bits.
    std::uint64_t next() SUPPLE_RT {
        const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
        const std::uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return result;
    }

    // Uniform double in [0, 1) with 53 bits of randomness.
    double uniform() SUPPLE_RT { return static_cast<double>(next() >> 11) * 0x1.0p-53; }

    // Uniform double in [lo, hi).
    double uniform(double lo, double hi) SUPPLE_RT { return lo + (hi - lo) * uniform(); }

    // Uniform float in [-1, 1) (audio noise).
    float bipolar() SUPPLE_RT {
        return static_cast<float>(static_cast<double>(next() >> 40) * 0x1.0p-23 - 1.0);
    }

    // Uniform integer in [0, n) without modulo bias (Lemire's method), n > 0.
    // Rejection sampling: each extra draw happens with probability < n / 2^64,
    // so the loop almost never runs (for n <= 2^32, below 2^-32 per call).
    std::uint64_t below(std::uint64_t n) SUPPLE_RT {
        assert(n > 0 && "Rng::below(0)");
        U128 m = static_cast<U128>(next()) * n;
        auto low = static_cast<std::uint64_t>(m);
        if (low < n) {
            const std::uint64_t threshold = (0 - n) % n;
            while (low < threshold) {
                m = static_cast<U128>(next()) * n;
                low = static_cast<std::uint64_t>(m);
            }
        }
        return static_cast<std::uint64_t>(m >> 64);
    }

    // Advances the stream by 2^128 steps: use to derive independent,
    // reproducible sub-streams (per voice, per worker) from one seed.
    void jump() SUPPLE_RT {
        static constexpr std::uint64_t kJump[] = {0x180ec6d33cfd0abaULL, 0xd5a61266f0c9392cULL,
                                                  0xa9582618e03fc9aaULL, 0x39abdc4529b1661cULL};
        std::uint64_t t[4] = {0, 0, 0, 0};
        for (std::uint64_t word : kJump) {
            for (int b = 0; b < 64; ++b) {
                if (word & (std::uint64_t{1} << b))
                    for (int i = 0; i < 4; ++i) t[i] ^= s_[i];
                next();
            }
        }
        for (int i = 0; i < 4; ++i) s_[i] = t[i];
    }

  private:
    static std::uint64_t rotl(std::uint64_t x, int k) SUPPLE_RT {
        return (x << k) | (x >> (64 - k));
    }
    static std::uint64_t splitmix64(std::uint64_t& x) SUPPLE_RT {
        std::uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

    std::uint64_t s_[4]{};
};

}  // namespace supple
