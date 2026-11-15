// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once

#include <algorithm>
#include <cstdio>
#include <limits>

#include "supple/audio/wav_stream.h"

namespace supple::audio {
// Per-writer faults, with real file bytes and no global I/O interception.
struct WavStreamWriterTestAccess {
    struct Fault {
        std::size_t bytes = std::numeric_limits<std::size_t>::max();
        bool buffered = false;
        bool failFlush = false;
    };
    static void inject(WavStreamWriter& writer, Fault& fault) {
        writer.io_.context = &fault;
        writer.io_.write = [](void* context, const void* data, std::size_t bytes, std::FILE* f) {
            auto& injected = *static_cast<Fault*>(context);
            const auto count = std::min(bytes, injected.bytes);
            const auto written = std::fwrite(data, 1, count, f);
            if (count < bytes) std::fflush(f);
            return injected.buffered ? bytes : written;
        };
        writer.io_.flush = [](void* context, std::FILE* f) {
            auto& injected = *static_cast<Fault*>(context);
            const int result = std::fflush(f);
            return injected.failFlush ? EOF : result;
        };
    }
};
}  // namespace supple::audio
