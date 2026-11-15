// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <limits>
#include <thread>

#include "supple/audio/wav_stream.h"
#include "supple/base/files.h"

namespace supple::audio {
// Per-writer faults, with real file bytes and no global I/O interception.
struct WavStreamWriterTestAccess {
    struct Fault {
        std::size_t bytes = std::numeric_limits<std::size_t>::max();
        bool buffered = false;
        bool failFlush = false;
        bool failSync = false;
        std::atomic<unsigned> syncCalls{0};
        std::thread::id syncThread;
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
        writer.io_.sync = [](void* context, std::FILE* f) {
            auto& injected = *static_cast<Fault*>(context);
            const bool synced = !injected.failSync && ::supple::syncFileStream(f);
            injected.syncThread = std::this_thread::get_id();
            injected.syncCalls.fetch_add(1, std::memory_order_release);
            injected.syncCalls.notify_all();
            return synced;
        };
    }
};
}  // namespace supple::audio
