// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "recording.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <tuple>

#include "supple/base/files.h"
#include "supple/base/thread.h"
#include "supple/json/json.h"

namespace supple::engine::detail {

std::filesystem::path sidecarPath(const std::filesystem::path& wav) {
    std::filesystem::path p = wav;
    p.replace_extension(".rec.json");
    return p;
}

Status writeSidecar(const std::filesystem::path& wav, const RecordingInfo& info) {
    json::Value v = json::Value::object();
    v["schema"] = "supple.recording/1.0";
    v["track"] = info.track.str();
    v["start_sample"] = static_cast<long long>(info.startSample);
    v["sample_rate"] = info.sampleRate;
    if (!info.hash.empty()) v["hash"] = info.hash;
    if (info.segments.size() > 1) {
        json::Value segs = json::Value::array();
        for (const RecordedSegment& sg : info.segments) {
            json::Value row = json::Value::array();
            row.push_back(static_cast<long long>(sg.fileFrame));
            row.push_back(static_cast<long long>(sg.timeline));
            segs.push_back(std::move(row));
        }
        v["segments"] = std::move(segs);
        json::Value loop = json::Value::array();
        loop.push_back(static_cast<long long>(info.loopStart));
        loop.push_back(static_cast<long long>(info.loopEnd));
        v["loop"] = std::move(loop);
    }
    return writeFileAtomic(sidecarPath(wav), json::writeCanonical(v) + "\n");
}

Result<RecordingInfo> readSidecar(const std::filesystem::path& wav) {
    auto text = readTextFile(sidecarPath(wav), 1 << 20);
    if (!text) return std::move(text).error();
    auto v = json::parse(*text);
    if (!v) return std::move(v).error();
    auto invalid = [&](std::string path, std::string message) {
        return Error("record.sidecar", sidecarPath(wav).string() + ": " + message, std::move(path),
                     "restore a valid supple.recording/1.0 sidecar or skip this file");
    };
    const auto* schema = v->find("schema");
    if (!v->isObject() || !schema || !schema->isString() ||
        schema->asString() != "supple.recording/1.0")
        return invalid("/schema", "unsupported recording schema/version");
    const auto* track = v->find("track");
    const auto* start = v->find("start_sample");
    const auto* rate = v->find("sample_rate");
    const auto id = track && track->isString() ? model::Id::parse(track->asString()) : std::nullopt;
    if (!id || id->kind != model::Kind::Track) return invalid("/track", "expected a tr_ track id");
    if (!start || !start->isInt()) return invalid("/start_sample", "expected an integer sample");
    if (!rate || !rate->isNumber() || !std::isfinite(rate->asDouble()) || rate->asDouble() <= 0)
        return invalid("/sample_rate", "expected a positive finite rate");
    RecordingInfo info{*id, start->asInt(), rate->asDouble(), {}, 0, 0};
    const auto* segs = v->find("segments");
    const auto* loop = v->find("loop");
    if (segs || loop) {
        if (!segs || !segs->isArray() || segs->size() == 0 || !loop || !loop->isArray() ||
            loop->size() != 2 || !loop->at(0).isInt() || !loop->at(1).isInt())
            return invalid("/segments", "loop and nonempty segments must be supplied together");
        info.loopStart = loop->at(0).asInt();
        info.loopEnd = loop->at(1).asInt();
        const bool noLoop = info.loopStart == 0 && info.loopEnd == 0;  // runs split by seeks
        if (!noLoop && (info.loopStart < 0 || info.loopEnd <= info.loopStart))
            return invalid("/loop", "expected [0, 0] or a nonnegative, increasing loop range");
        for (std::size_t i = 0; i < segs->size(); ++i) {
            const auto& row = segs->at(i);
            if (!row.isArray() || row.size() != 2 || !row.at(0).isInt() || !row.at(1).isInt() ||
                row.at(0).asInt() < 0 || (i && row.at(0).asInt() <= info.segments.back().fileFrame))
                return invalid("/segments/" + std::to_string(i),
                               "expected increasing nonnegative file frames");
            info.segments.push_back({row.at(0).asInt(), row.at(1).asInt()});
        }
        if (info.segments.front().fileFrame != 0 ||
            info.segments.front().timeline != info.startSample)
            return invalid("/segments/0",
                           "the first run must begin at file frame 0 and start_sample");
    }
    return info;
}

void FrameRing::init(std::uint32_t channels, std::size_t frames) {
    channels_ = channels;
    capacity_ = frames;
    buf_.assign(frames * channels, 0.0f);
    positions_.assign(frames, 0);
    write_ = 0;
    read_ = 0;
}

std::size_t FrameRing::write(const float* const* in, std::uint32_t inChannels,
                             std::span<const int> select, std::uint32_t frames,
                             std::int64_t firstFrame) SUPPLE_RT {
    const std::size_t w = write_.load(std::memory_order_relaxed);
    const std::size_t room = capacity_ - (w - read_.load(std::memory_order_acquire));
    const std::size_t n = std::min<std::size_t>(frames, room);
    for (std::size_t i = 0; i < n; ++i) {
        float* dst = buf_.data() + ((w + i) % capacity_) * channels_;
        positions_[(w + i) % capacity_] = firstFrame + static_cast<std::int64_t>(i);
        for (std::uint32_t c = 0; c < channels_; ++c) {
            const int src = c < select.size() ? select[c] : -1;
            dst[c] = in && src >= 0 && static_cast<std::uint32_t>(src) < inChannels && in[src]
                         ? in[src][i]
                         : 0.0f;
        }
    }
    write_.store(w + n, std::memory_order_release);
    return n;
}

std::size_t FrameRing::read(float* out, std::size_t maxFrames, std::int64_t* firstFrame) {
    const std::size_t r = read_.load(std::memory_order_relaxed);
    const std::size_t available = std::min(maxFrames, write_.load(std::memory_order_acquire) - r);
    if (available && firstFrame) *firstFrame = positions_[r % capacity_];
    std::size_t n = 0;
    for (; n < available; ++n) {
        if (firstFrame &&
            positions_[(r + n) % capacity_] != *firstFrame + static_cast<std::int64_t>(n))
            break;
        std::copy_n(buf_.data() + ((r + n) % capacity_) * channels_, channels_,
                    out + n * channels_);
    }
    read_.store(r + n, std::memory_order_release);
    return n;
}

Recorder::Recorder(std::vector<RecordTarget> targets, double rate, std::int64_t latency)
    : rate_(rate), latency_(latency) {
    for (RecordTarget& t : targets) {
        if (t.midi) {
            auto track = std::make_unique<MidiTrack>();
            track->target = std::move(t);
            midiTracks_.push_back(std::move(track));
            continue;
        }
        auto track = std::make_unique<Track>();
        track->target = std::move(t);
        track->ring.init(
            track->target.output || track->target.rackInput
                ? 2u
                : static_cast<std::uint32_t>(std::max<std::size_t>(1, track->target.inputs.size())),
            static_cast<std::size_t>(rate * 10.0));  // ten seconds
        // A rendered signal (a master's output or a rack input) at P belongs to
        // the timeline at P - outputShift, device input at P - latency: its
        // frames go that much later in its file (earlier when negative, the
        // first ones dropped), so every file starts at the same position.
        if (track->target.output || track->target.rackInput) {
            track->shift = track->target.outputShift - latency_;
            track->skip = std::max<std::int64_t>(0, track->shift);
        }
        tracks_.push_back(std::move(track));
    }
}

Recorder::~Recorder() {
    stop_ = true;
    if (writer_.joinable()) writer_.join();
}

Status Recorder::open() {
    for (auto& t : tracks_) {
        std::error_code ec;
        std::filesystem::create_directories(t->target.file.parent_path(), ec);
        SUPPLE_TRY(t->writer.open(t->target.file, rate_, t->ring.channels()));
    }
    writer_ = std::thread([this] { writerLoop(); });
    return okStatus();
}

void Recorder::capture(const float* const* in, std::uint32_t inChannels, std::uint32_t frames,
                       std::int64_t pos, std::span<const ProcessedInput> processed) SUPPLE_RT {
    if (frames == 0 || runsFull_) return;
    if (!started_.load(std::memory_order_relaxed)) {
        // What the performer heard at `pos` reaches us a round trip later.
        start_.store(pos - latency_, std::memory_order_relaxed);
        started_.store(true, std::memory_order_release);
    } else if (pos != expected_ &&  // a loop wrap: a new run on the timeline
               !(driftTolerant_ && std::abs(pos - expected_) < static_cast<std::int64_t>(frames))) {
        const std::size_t n = segmentCount_.load(std::memory_order_relaxed);
        if (n < kMaxSegments) {
            segments_[n] = {captured_, pos};
            segmentCount_.store(n + 1, std::memory_order_release);
        } else {
            runsFull_ = true;
            return;
        }
    }
    expected_ = pos + frames;
    static constexpr int kPair[2] = {0, 1};
    for (auto& t : tracks_) {
        const ProcessedInput* own = nullptr;
        for (const ProcessedInput& p : processed)
            if (p.track == t->target.track) {
                own = &p;
                break;
            }
        if (t->target.output ||
            t->target.rackInput) {  // rendered signal; none this part: pad silence
            const auto from = static_cast<std::uint32_t>(std::min<std::int64_t>(frames, t->skip));
            t->skip -= from;
            if (!own || from == frames) continue;
            const float* channels[2] = {own->channels[0] + from, own->channels[1] + from};
            const std::uint32_t count = frames - from;
            const std::size_t n =
                t->ring.write(channels, 2, kPair, count, captured_ + from - t->shift);
            if (n < count) t->overflow.fetch_add(count - n, std::memory_order_relaxed);
            continue;
        }
        if (!own) {
            const std::size_t n =
                t->ring.write(in, inChannels, t->target.inputs, frames, captured_);
            if (n < frames) t->overflow.fetch_add(frames - n, std::memory_order_relaxed);
            continue;
        }
        if (captured_ == 0) t->skip = t->shift = own->latency;
        const auto from = static_cast<std::uint32_t>(std::min<std::int64_t>(frames, t->skip));
        t->skip -= from;
        if (from == frames) continue;
        const float* channels[2] = {own->channels[0] + from, own->channels[1] + from};
        const std::span<const int> select(kPair, std::min<std::uint32_t>(t->ring.channels(), 2));
        const std::uint32_t count = frames - from;
        const std::size_t n =
            t->ring.write(channels, 2, select, count, captured_ + from - t->shift);
        if (n < count) t->overflow.fetch_add(count - n, std::memory_order_relaxed);
    }
    captured_ += frames;
}

void Recorder::captureMidi(std::span<const MidiInputEvent> events, std::uint32_t offset,
                           std::uint32_t frames, std::int64_t pos) SUPPLE_RT {
    (void)pos;  // placed by finish() from the captured position and the runs
    if (runsFull_ || midiTracks_.empty() || events.empty() ||
        !started_.load(std::memory_order_relaxed))
        return;
    // capture() has counted this part: it began at captured_ - frames.
    const std::int64_t base = captured_ - frames;
    for (const MidiInputEvent& ev : events) {
        if (ev.frame < offset) continue;
        if (ev.frame >= offset + frames) break;
        for (auto& t : midiTracks_) {
            std::uint8_t m[3];
            if (!routeMidi(ev, t->target.midi->ports, t->target.midi->fromChannel,
                           t->target.midi->toChannel, m))
                continue;
            MidiEvent slot;
            slot.at = base + (ev.frame - offset);
            slot.at += midiClock(*t) - latency_;
            std::copy_n(m, 3, slot.bytes);
            pushMidi(*t, slot);
        }
    }
}

std::int64_t Recorder::midiClock(const MidiTrack& track) const SUPPLE_RT {
    const auto& midi = *track.target.midi;
    if (!midi.routed) return latency_;
    return midi.ports ? std::max(latency_, midi.shift) : midi.shift;
}

void Recorder::pushMidi(MidiTrack& track, const MidiEvent& event) SUPPLE_RT {
    const auto w = track.write.load(std::memory_order_relaxed);
    if (w - track.read.load(std::memory_order_acquire) >= MidiTrack::kRing) {
        track.overflow.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    track.ring[w % MidiTrack::kRing] = event;
    track.write.store(w + 1, std::memory_order_release);
}

void Recorder::captureRoutedMidi(model::Id track, std::span<const RoutedMidiEvent> events,
                                 std::uint32_t frames) SUPPLE_RT {
    if (runsFull_ || !started_.load(std::memory_order_relaxed)) return;
    for (auto& t : midiTracks_) {
        if (t->target.track != track || !t->target.midi->routed) continue;
        for (const auto& event : events) {
            MidiEvent slot;
            slot.at = captured_ - frames + event.frame + midiClock(*t) - t->target.midi->shift;
            slot.route = event.route;
            slot.voice = event.voice;
            std::copy_n(event.bytes, 3, slot.bytes);
            pushMidi(*t, slot);
        }
    }
}

void Recorder::drainMidi() {
    for (auto& t : midiTracks_) {
        const std::size_t end = t->write.load(std::memory_order_acquire);
        for (std::size_t r = t->read.load(std::memory_order_relaxed); r != end; ++r)
            t->drained.push_back(t->ring[r % MidiTrack::kRing]);
        t->read.store(end, std::memory_order_release);
    }
}

void Recorder::lose(std::uint32_t frames) SUPPLE_RT {
    if (runsFull_) return;
    if (frames == 0 || !started_.load(std::memory_order_relaxed)) return;
    for (auto& t : tracks_) t->overflow.fetch_add(frames, std::memory_order_relaxed);
}

void Recorder::padTo(Track& t, std::int64_t frame) {
    const std::array<float, 4096> silence{};
    while (t.writeError && t.writer.frames() < static_cast<std::uint64_t>(frame)) {
        const auto n =
            std::min<std::uint64_t>(silence.size() / t.ring.channels(),
                                    static_cast<std::uint64_t>(frame) - t.writer.frames());
        auto st = t.writer.append(silence.data(), static_cast<std::size_t>(n));
        rememberError(t, std::move(st));
    }
}

void Recorder::drain(Track& t, std::vector<float>& scratch) {
    const std::size_t chunk = scratch.size() / t.ring.channels();
    for (;;) {
        std::int64_t first = 0;
        const std::size_t n = t.ring.read(scratch.data(), chunk, &first);
        if (n == 0) break;
        padTo(t, first);
        if (t.writeError) {
            auto st = t.writer.append(scratch.data(), n);
            rememberError(t, std::move(st));
        }
        if (!t.writeError) t.overflow.fetch_add(n, std::memory_order_relaxed);  // lost too
    }
    if (t.writeError) {
        auto st = t.writer.flush();  // a crash of the process keeps what the OS has
        rememberError(t, std::move(st));
    }
}

void Recorder::writeSidecars() {
    if (sidecars_ || !started_.load(std::memory_order_acquire)) return;
    sidecars_ = true;
    const std::int64_t start = start_.load(std::memory_order_relaxed);
    for (auto& t : tracks_) {
        // Without one the take is fine but a crash would leave it unplaced.
        auto st = writeSidecar(t->target.file, {t->target.track, start, rate_, {}, 0, 0});
        rememberError(*t, std::move(st), true);
    }
}

void Recorder::rememberError(Track& track, Status status, bool sidecar) {
    if (status) return;
    if (track.firstError) track.firstError = status.error();
    Status& target = sidecar ? track.sidecarError : track.writeError;
    if (target) target = std::move(status);
}

void Recorder::updateHeaders() {
    for (auto& t : tracks_)
        if (t->writeError) rememberError(*t, t->writer.updateHeader());
}

void Recorder::writerLoop() {
    nameThread("audio-recorder");
    std::vector<float> scratch(8192 * 16);
    auto lastHeader = std::chrono::steady_clock::now();
    while (!stop_.load(std::memory_order_acquire)) {
        if (writerStalled_.load(std::memory_order_acquire)) {
            writerStallAck_.store(true, std::memory_order_release);
            while (writerStalled_.load(std::memory_order_acquire) && !stop_.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            writerStallAck_.store(false, std::memory_order_release);
        }
        writeSidecars();
        for (auto& t : tracks_) drain(*t, scratch);
        drainMidi();  // the MIDI rings too (bounded: a long performance must not overflow)
        if (std::chrono::steady_clock::now() - lastHeader > std::chrono::seconds(5)) {
            updateHeaders();  // a crash keeps this much
            lastHeader = std::chrono::steady_clock::now();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    writeSidecars();
    for (auto& t : tracks_) drain(*t, scratch);    // what arrived before the stop
    for (auto& t : tracks_) padTo(*t, captured_);  // a final dropped tail has no next timestamp
    // Persist complete placement before finish returns, so recovery of a
    // stopped-but-unhashed loop take still has every pass. No main-thread I/O.
    if (started_.load(std::memory_order_acquire)) {
        const auto start = start_.load(std::memory_order_relaxed);
        const auto count = segmentCount_.load(std::memory_order_acquire);
        for (auto& t : tracks_) {
            RecordingInfo info{t->target.track,   start, rate_, {{0, start}}, finishingLoop_.start,
                               finishingLoop_.end};
            for (std::size_t i = 0; i < count; ++i) {
                const auto frame = segments_[i].fileFrame + latency_;
                if (frame < static_cast<std::int64_t>(t->writer.frames()))
                    info.segments.push_back({frame, segments_[i].timeline});
            }
            if (info.segments.size() > 1)
                rememberError(*t, writeSidecar(t->target.file, info), true);
        }
    }
}

std::vector<RecordedTake> Recorder::finish(LoopRange loop) {
    // Runs split by a seek, not a loop, carry no loop range ([0, 0]).
    if (!loop.active()) loop = {};
    finishingLoop_ = loop;
    stop_.store(true, std::memory_order_release);
    if (writer_.joinable()) writer_.join();
    std::vector<RecordedTake> out;
    const std::int64_t start =
        started_.load(std::memory_order_acquire) ? start_.load(std::memory_order_relaxed) : 0;
    // A run captured at output position P from file frame F was heard a
    // round trip earlier: its first frame on the timeline is F + latency.
    std::vector<RecordedSegment> segments = {{0, start}};
    const std::size_t count = segmentCount_.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < count; ++i)
        segments.push_back({segments_[i].fileFrame + latency_, segments_[i].timeline});
    // MIDI (ADR-0016): each event was played `latency` frames before it was
    // captured; the run holding that moment gives its timeline position.
    drainMidi();
    for (auto& t : midiTracks_) {
        RecordedTake r;
        r.track = t->target.track;
        r.isMidi = true;
        r.startSample = start;
        r.sampleRate = rate_;
        r.frames = captured_;
        const auto& midi = *t->target.midi;
        const auto clock = midiClock(*t);
        r.startSample += latency_ - clock;
        if (midi.routed)
            r.frames += clock - (midi.ports ? std::min(latency_, midi.shift) : midi.shift);
        if (r.frames > 0) r.segments.push_back({0, r.startSample});
        for (std::size_t i = 0; i < count; ++i) {
            const auto boundary = segments_[i].fileFrame + clock;
            if (boundary < r.frames) r.segments.push_back({boundary, segments_[i].timeline});
        }
        r.loop = {loop.start, loop.end};
        r.overflowFrames = t->overflow.load();
        auto append = [&](const MidiEvent& ev) {
            // ev.at counts captured frames like an audio file's frames; the
            // runs are in those units, already shifted by the latency.
            std::size_t k = 0;
            while (k + 1 < r.segments.size() && r.segments[k + 1].fileFrame <= ev.at) ++k;
            const RecordedSegment sg =
                r.segments.empty() ? RecordedSegment{0, r.startSample} : r.segments[k];
            RecordedMidiEvent e;
            e.timeline = sg.timeline + (ev.at - sg.fileFrame);
            e.run = static_cast<std::uint32_t>(k);
            std::copy_n(ev.bytes, 3, e.bytes);
            e.route = ev.route;
            e.voice = ev.voice;
            r.midi.push_back(e);
        };
        std::map<std::tuple<model::Id, int, int, int>, MidiEvent> held;
        for (const MidiEvent& ev : t->drained) {
            append(ev);
            if (midi.routed && ev.bytes[1] < 128) {
                const int kind = ev.bytes[0] & 0xf0;
                const auto key = std::tuple{ev.route, ev.voice, ev.bytes[0] & 15, int(ev.bytes[1])};
                if (kind == 0x90 && ev.bytes[2] != 0)
                    held[key] = ev;
                else if (kind == 0x80 || kind == 0x90)
                    held.erase(key);
            }
        }
        if (midi.routed) {
            // Each source ends on its own clock, even when the take spans
            // both endpoints (device latency and rendered input shift).
            for (auto& [key, off] : held) {
                (void)key;
                off.at = captured_ + clock - (off.voice < 0 ? latency_ : midi.shift);
                off.bytes[0] = static_cast<std::uint8_t>(0x80 | (off.bytes[0] & 15));
                off.bytes[2] = 0;
                append(off);
            }
            std::stable_sort(r.midi.begin(), r.midi.end(), [](const auto& a, const auto& b) {
                const auto rank = [](const auto& e) {
                    const int kind = e.bytes[0] & 0xf0;
                    return kind == 0x80 || (kind == 0x90 && e.bytes[2] == 0) ? 0 : 1;
                };
                return std::tuple{a.run, a.timeline, rank(a)} <
                       std::tuple{b.run, b.timeline, rank(b)};
            });
        }
        if (runsFull_)
            r.warnings.emplace_back(
                "recording.loop_runs_full",
                "recording stopped adding input because its loop run table is full", "/recording",
                "commit the retained passes and start a new recording");
        out.push_back(std::move(r));
    }
    for (auto& t : tracks_) {
        RecordedTake r;
        r.track = t->target.track;
        r.file = t->target.file;
        r.startSample = start;
        r.sampleRate = rate_;
        r.frames = static_cast<std::int64_t>(t->writer.frames());
        for (const RecordedSegment& sg : segments)
            if (sg.fileFrame < r.frames) r.segments.push_back(sg);
        r.loop = {loop.start, loop.end};
        r.overflowFrames = t->overflow.load();
        rememberError(*t, t->writer.close());
        if (!t->firstError) r.error = t->firstError.error().toString();
        if (runsFull_)
            r.warnings.emplace_back(
                "recording.loop_runs_full",
                "recording stopped adding input because its loop run table is full", "/recording",
                "commit the retained passes and start a new recording");
        out.push_back(std::move(r));
    }
    return out;
}

}  // namespace supple::engine::detail
