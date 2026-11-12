// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "midi_output.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include "supple/base/thread.h"
namespace supple::engine::detail {
namespace {
bool valid(const MidiOutputEvent& e) SUPPLE_RT {
    if (!e.size || e.size > 10 || e.bytes[0] < 0x80) return false;
    const auto status = e.bytes[0];
    if (status == 0xf0) {
        if (e.size < 2 || e.bytes[e.size - 1] != 0xf7) return false;
        for (unsigned i = 1; i + 1 < e.size; ++i)
            if (e.bytes[i] >= 0x80) return false;
        return true;
    }
    unsigned size = 0;
    if (status < 0xf0)
        size = (status & 0xe0) == 0xc0 ? 2 : 3;
    else if (status == 0xf1 || status == 0xf3)
        size = 2;
    else if (status == 0xf2)
        size = 3;
    else if (status == 0xf6 || status == 0xf8 || status == 0xfa || status == 0xfb ||
             status == 0xfc || status == 0xfe || status == 0xff)
        size = 1;
    if (size != e.size) return false;
    for (unsigned i = 1; i < size; ++i)
        if (e.bytes[i] >= 0x80) return false;
    return true;
}
}  // namespace
void MidiOutputCycle::begin(std::uint32_t frames, std::size_t ports) SUPPLE_RT {
    count_ = 0;
    ports_ = ports;
    chunk(0, frames);
}
void MidiOutputCycle::chunk(std::uint32_t offset, std::uint32_t frames) SUPPLE_RT {
    offset_ = offset;
    frames_ = frames;
}
bool MidiOutputCycle::write(const MidiOutputEvent& event) SUPPLE_RT {
    if (event.port >= ports_ || event.frame >= frames_ || count_ == events_.size() ||
        !valid(event)) {
        drop();
        return false;
    }
    events_[count_] = event;
    events_[count_++].frame += offset_;
    return true;
}
std::span<const MidiOutputEvent> MidiOutputCycle::sorted() SUPPLE_RT {
    for (std::size_t i = 1; i < count_; ++i)
        for (std::size_t k = i; k && events_[k].frame < events_[k - 1].frame; --k)
            std::swap(events_[k], events_[k - 1]);
    frames_ = 0;  // writes outside the callback fail
    return {events_.data(), count_};
}
MidiOutputUmp midiOutputUmp(const MidiOutputEvent& e) SUPPLE_RT {
    MidiOutputUmp result;
    if (!valid(e)) return result;
    if (e.bytes[0] != 0xf0) {
        result.count = 1;
        result.words[0] =
            (e.bytes[0] < 0xf0 ? 0x20000000u : 0x10000000u) | std::uint32_t(e.bytes[0]) << 16;
        if (e.size > 1) result.words[0] |= std::uint32_t(e.bytes[1]) << 8;
        if (e.size > 2) result.words[0] |= e.bytes[2];
        return result;
    }
    const unsigned length = e.size - 2;
    for (unsigned at = 0; at < length || at == 0; at += 6) {
        const unsigned count = std::min(6u, length - at);
        const unsigned status = length <= 6 ? 0 : at == 0 ? 1 : 3;
        auto* words = result.words.data() + result.count;
        words[0] = 0x30000000u | status << 20 | count << 16;
        for (unsigned b = 0; b < count; ++b) {
            if (b < 2)
                words[0] |= std::uint32_t(e.bytes[1 + at + b]) << (8 * (1 - b));
            else
                words[1] |= std::uint32_t(e.bytes[1 + at + b]) << (8 * (5 - b));
        }
        result.count += 2;
        if (!length) break;
    }
    return result;
}
bool closingMessage(const MidiOutputEvent& e) SUPPLE_RT {
    const auto status = e.bytes[0] & 0xf0;
    return status == 0x80 || (status == 0x90 && !e.bytes[2]) ||
           (status == 0xb0 && (e.bytes[1] == 64 || e.bytes[1] == 123) && !e.bytes[2]) ||
           (e.size == 1 && e.bytes[0] == 0xfc);  // a clock slave's Stop (ADR-0073 §9)
}
double midiOutputCloseDeadline(double now, double lastScheduled) {
    return now + std::clamp(lastScheduled - now, 0.0, kMidiOutputCloseWaitSeconds);
}
void MidiSink::waitUntil(double seconds) {
    const double delay = std::clamp(seconds - now(), 0.0, .001);
    if (delay > 0) std::this_thread::sleep_for(std::chrono::duration<double>(delay));
}
Status MidiOutputSender::start() {
    if (thread_.joinable())
        return Error("backend.midi", "MIDI sender is already running", "/midi_outputs",
                     "stop before restarting the sender");
    lastDue_.fill(0);
    lastSent_ = 0;
    dropped_.store(0);
    for (auto& dropped : portDrops_) dropped.store(0);
    quit_.store(false);
    try {
        thread_ = std::thread([this] {
            nameThread("midi-output");
            sink_.prepareThread();
            while (!quit_.load(std::memory_order_acquire)) sink_.waitUntil(dispatch());
            sink_.releaseThread();
        });
    } catch (const std::system_error& error) {
        return Error("backend.midi", error.what(), "/midi_outputs",
                     "reduce the thread count and restart the engine");
    }
    return okStatus();
}
void MidiOutputSender::stop() {
    quit_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    TimedMidiOutput event;
    for (auto& queue : queues_)
        while (queue.pop(event)) {
            if (closingMessage(event.event)) {
                event.seconds = sink_.timestamped()
                                    ? std::max(sink_.now(), lastDue_[event.event.port])
                                    : sink_.now();
                if (sink_.send(event))
                    lastSent_ = std::max(lastSent_, event.seconds);
                else
                    drop(event.event.port);
            } else
                drop(event.event.port);
        }
    if (sink_.timestamped()) {
        const double deadline = midiOutputCloseDeadline(sink_.now(), lastSent_);
        const auto limit = std::chrono::steady_clock::now() +
                           std::chrono::duration<double>(kMidiOutputCloseWaitSeconds);
        // Also bound a broken/stationary sink clock; the OS wait itself is
        // polled at most 1 ms. Device-free sinks need not advance with wall time.
        while (sink_.now() < deadline && std::chrono::steady_clock::now() < limit)
            sink_.waitUntil(deadline);
    }
}
bool MidiOutputSender::push(const TimedMidiOutput& event) SUPPLE_RT {
    const auto port = event.event.port;
    if (port >= sink_.names.size() || port >= queues_.size() || !std::isfinite(event.seconds)) {
        drop(event.event.port);
        return false;
    }
    auto queued = event;
    const bool release = closingMessage(event.event);
    if (queued.closing)
        queued.seconds = sink_.timestamped() ? std::max(sink_.now(), lastDue_[port]) : sink_.now();
    else if (release)
        queued.seconds = std::max(queued.seconds, lastDue_[port]);
    if ((!queued.closing && queued.seconds < lastDue_[port]) || !queues_[port].push(queued)) {
        drop(event.event.port);
        return false;
    }
    if (!queued.closing) lastDue_[port] = queued.seconds;
    return true;
}
double MidiOutputSender::dispatch() {
    // Bounded work; pick the earliest head across ports, stable by port index.
    for (std::size_t n = 0; n < kMidiOutputQueueSize * kMidiOutputPorts; ++n) {
        if (quit_.load(std::memory_order_acquire)) return sink_.now();
        TimedMidiOutput next, candidate;
        std::size_t selected = queues_.size();
        for (std::size_t p = 0; p < sink_.names.size() && p < queues_.size(); ++p)
            if (queues_[p].peek(candidate) &&
                (selected == queues_.size() || candidate.seconds < next.seconds)) {
                selected = p;
                next = candidate;
            }
        if (selected == queues_.size()) return sink_.now() + .001;
        if (!sink_.timestamped() && !next.closing && next.seconds > sink_.now())
            return next.seconds;
        if (next.closing) next.seconds = std::max(next.seconds, sink_.now());
        if (sink_.send(next))
            lastSent_ = std::max(lastSent_, next.seconds);
        else
            drop(next.event.port);
        queues_[selected].consume();
    }
    return sink_.now();
}
#if !defined(__APPLE__) && !defined(_WIN32)
std::unique_ptr<MidiSink> nativeMidiSink(MidiSource*) { return {}; }
#endif
}  // namespace supple::engine::detail
