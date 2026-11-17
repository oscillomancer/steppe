// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "recording.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <map>
#include <tuple>

#include "file_platform.h"
#include "media_files.h"
#include "supple/base/files.h"
#include "supple/base/main_thread_wake.h"
#include "supple/base/thread.h"
#include "supple/diagctx/table.h"
#include "supple/json/json.h"

namespace supple::engine::detail {

namespace {
json::Value punchJson(RecordedPunch punch) {
    auto v = json::Value::object();
    v["enabled"] = punch.enabled;
    v["start"] = static_cast<long long>(punch.start);
    v["end"] = static_cast<long long>(punch.end);
    return v;
}
std::optional<RecordedPunch> parsePunch(const json::Value* value) {
    if (!value || !value->isObject()) return {};
    const auto* enabled = value->find("enabled");
    const auto* start = value->find("start");
    const auto* end = value->find("end");
    auto bounded = [](const json::Value* v) {
        return v && v->isInt() && v->asInt() >= 0 && v->asInt() <= kRecordingClockLimit;
    };
    if (!enabled || !enabled->isBool() || !bounded(start) || !bounded(end) ||
        (enabled->asBool() && end->asInt() <= start->asInt()))
        return {};
    return RecordedPunch{enabled->asBool(), start->asInt(), end->asInt()};
}
}  // namespace

Status validateRecordingPunch(RecordedPunch punch) {
    if (punch.enabled && punch.end <= punch.start)
        return Error(
            "record.punch", "the enabled punch contains no recording sample", "/punch/end",
            "set the end at least one recording sample after the start, or clear the punch");
    if (punch.start < 0 || punch.end < 0 || punch.start > kRecordingClockLimit ||
        punch.end > kRecordingClockLimit)
        return Error("record.punch", "the punch is outside the recording clock range", "/punch",
                     "clear the punch or use a nonnegative range within 2^48 recording frames");
    return okStatus();
}

std::filesystem::path sidecarPath(const std::filesystem::path& wav) {
    std::filesystem::path p = wav;
    p.replace_extension(".rec.json");
    return p;
}

Status writeSidecar(const std::filesystem::path& wav, const RecordingInfo& info) {
    json::Value v = json::Value::object();
    v["schema"] = info.punch    ? "supple.recording/1.2"
                  : info.frames ? "supple.recording/1.1"
                                : "supple.recording/1.0";
    if (info.punch) v["punch"] = punchJson(*info.punch);
    if (info.frames) v["frames"] = static_cast<long long>(*info.frames);
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

Result<RecordingInfo> readSidecar(const std::filesystem::path& wav,
                                  const std::filesystem::path& root) {
    auto text = readRecoveryText(root, sidecarPath(wav), 1 << 20);
    if (!text) return std::move(text).error();
    auto v = json::parse(*text);
    if (!v) return std::move(v).error();
    auto invalid = [&](std::string path, std::string message) {
        return Error("record.sidecar", sidecarPath(wav).string() + ": " + message, std::move(path),
                     "restore a valid recording sidecar or skip this file");
    };
    const auto* schema = v->find("schema");
    if (!v->isObject() || !schema || !schema->isString() ||
        (schema->asString() != "supple.recording/1.0" &&
         schema->asString() != "supple.recording/1.1" &&
         schema->asString() != "supple.recording/1.2"))
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
    if (schema->asString() == "supple.recording/1.2") {
        info.punch = parsePunch(v->find("punch"));
        if (!info.punch) return invalid("/punch", "expected a bounded punch range");
    }
    if (schema->asString() == "supple.recording/1.1" ||
        (schema->asString() == "supple.recording/1.2" && v->find("frames"))) {
        const auto* frames = v->find("frames");
        if (!frames || !frames->isInt() || frames->asInt() < 0 ||
            frames->asInt() > kRecordingClockLimit)
            return invalid("/frames", "expected a bounded nonnegative frame count");
        info.frames = frames->asInt();
    }
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

std::int64_t midiClock(const RecordTarget::Midi& midi, std::int64_t latency) {
    if (!midi.routed) return latency;
    return midi.ports ? std::max(latency, midi.shift) : midi.shift;
}

RecordedTake placeMidi(const MidiCapture& c) {
    RecordedTake r;
    r.track = c.track;
    r.isMidi = true;
    r.punch = c.punch;
    r.startSample = c.start;
    r.sampleRate = c.rate;
    r.frames = c.captured;
    const auto& midi = c.midi;
    const auto clock = midiClock(midi, c.latency);
    // MIDI (ADR-0016): each event was played `latency` frames before it was
    // captured; the run holding that moment gives its timeline position.
    r.startSample += c.latency - clock;
    if (midi.routed)
        r.frames += clock - (midi.ports ? std::min(c.latency, midi.shift) : midi.shift);
    if (r.frames > 0) r.segments.push_back({0, r.startSample});
    for (const RecordedSegment& run : c.runs) {
        const auto boundary = run.fileFrame + clock;
        if (boundary < r.frames) r.segments.push_back({boundary, run.timeline});
    }
    const LoopRange loop = c.loop.active() ? c.loop : LoopRange{};
    r.loop = {loop.start, loop.end};
    r.overflowFrames = c.overflow;
    auto append = [&](const CapturedMidiEvent& ev) {
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
    std::map<std::tuple<model::Id, int, int, int>, CapturedMidiEvent> held;
    for (const CapturedMidiEvent& ev : c.events) {
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
            off.at = c.captured + clock - (off.voice < 0 ? c.latency : midi.shift);
            off.bytes[0] = static_cast<std::uint8_t>(0x80 | (off.bytes[0] & 15));
            off.bytes[2] = 0;
            append(off);
        }
        std::stable_sort(r.midi.begin(), r.midi.end(), [](const auto& a, const auto& b) {
            const auto rank = [](const auto& e) {
                const int kind = e.bytes[0] & 0xf0;
                return kind == 0x80 || (kind == 0x90 && e.bytes[2] == 0) ? 0 : 1;
            };
            return std::tuple{a.run, a.timeline, rank(a)} < std::tuple{b.run, b.timeline, rank(b)};
        });
    }
    if (c.runsFull)
        r.warnings.emplace_back("recording.loop_runs_full",
                                "recording stopped adding input because its loop run table is full",
                                "/recording",
                                "commit the retained passes and start a new recording");
    return r;
}

namespace {
constexpr std::string_view kJournalSuffix = ".midi.jsonl";
constexpr std::string_view kJournalSchema = "supple.midi-recording/1.1";
constexpr std::int64_t kJournalClockLimit = kRecordingClockLimit;
bool clock(const json::Value& v, std::int64_t low = -kJournalClockLimit) {
    return v.isInt() && v.asInt() >= low && v.asInt() <= kJournalClockLimit;
}

// One value per line: compact (keys in a fixed order), never a newline inside.
std::string journalLine(const json::Value& v) { return json::writeCompact(v) + "\n"; }
}  // namespace

bool isMidiJournal(const std::filesystem::path& file) {
    return file.filename().string().ends_with(kJournalSuffix);
}

std::filesystem::path audioJournalPath(const std::filesystem::path& wav) {
    auto path = wav;
    path.replace_extension(".audio.jsonl");
    return path;
}
bool isAudioJournal(const std::filesystem::path& file) {
    return file.filename().string().ends_with(".audio.jsonl");
}

static Result<MidiCapture> readJournal(const std::filesystem::path& file,
                                       const std::filesystem::path& root, bool* torn, bool audio) {
    auto text = readRecoveryText(root, file, std::size_t(1) << 30);
    if (!text) return std::move(text).error();
    auto invalid = [&](std::string path, std::string message) {
        return Error("record.journal", file.string() + ": " + message, std::move(path),
                     "skip this file; it is not a readable recording journal");
    };
    if (torn) *torn = false;
    MidiCapture c;
    bool header = false, punchAware = false;
    std::size_t at = 0;
    while (at < text->size()) {
        const std::size_t end = text->find('\n', at);
        if (end == std::string::npos) {  // the write a crash interrupted
            if (torn) *torn = true;
            break;
        }
        auto v = json::parse(std::string_view(*text).substr(at, end - at));
        at = end + 1;
        if (!header) {
            const auto* schema = v ? v->find("schema") : nullptr;
            if (!v || !v->isObject() || !schema || !schema->isString() ||
                (schema->asString() != (audio ? "supple.audio-recording/1.1" : kJournalSchema) &&
                 schema->asString() !=
                     (audio ? "supple.audio-recording/1.0" : "supple.midi-recording/1.0")))
                return invalid("/0/schema", "unsupported recording journal schema/version");
            const auto* track = v->find("track");
            const auto id =
                track && track->isString() ? model::Id::parse(track->asString()) : std::nullopt;
            if (!id || id->kind != model::Kind::Track)
                return invalid("/0/track", "expected a tr_ track id");
            auto integer = [&](const char* name, std::int64_t& out,
                               std::int64_t low = -kJournalClockLimit) {
                const auto* f = v->find(name);
                if (!f || !clock(*f, low)) return false;
                out = f->asInt();
                return true;
            };
            auto flag = [&](const char* name, bool& out) {
                const auto* f = v->find(name);
                if (!f || !f->isBool()) return false;
                out = f->asBool();
                return true;
            };
            const auto* rate = v->find("sample_rate");
            const auto* loop = v->find("loop");
            bool device = false;
            if (!rate || !rate->isNumber() || !std::isfinite(rate->asDouble()) ||
                rate->asDouble() <= 0 || !integer("start_sample", c.start) ||
                !integer("latency", c.latency, 0) ||
                (!audio && (!integer("shift", c.midi.shift) || !flag("device", device) ||
                            !flag("routed", c.midi.routed) || !(device || c.midi.routed))) ||
                !loop || !loop->isArray() || loop->size() != 2 || !clock(loop->at(0), 0) ||
                !clock(loop->at(1), 0) ||
                (loop->at(1).asInt() <= loop->at(0).asInt() &&
                 (loop->at(0).asInt() != 0 || loop->at(1).asInt() != 0)))
                return invalid("/0",
                               "expected sample_rate, start_sample, latency, shift, device, "
                               "routed and loop, within the range a recording can have");
            if (audio) {
                const auto* channels = v->find("channels");
                if (!channels || !channels->isInt() || channels->asInt() < 1 ||
                    channels->asInt() > 64)
                    return invalid("/0/channels", "expected 1..64 audio channels");
                c.channels = static_cast<std::uint32_t>(channels->asInt());
            }
            punchAware = schema->asString().ends_with("/1.1");
            if (punchAware) {
                auto punch = parsePunch(v->find("punch"));
                if (!punch) return invalid("/0/punch", "expected a bounded punch range");
                c.punch = *punch;
            }
            c.track = *id;
            c.rate = rate->asDouble();
            c.midi.ports = device ? 1 : 0;
            c.loop = {loop->at(0).asInt(), loop->at(1).asInt(),
                      loop->at(1).asInt() > loop->at(0).asInt()};
            header = true;
            continue;
        }
        // A line that does not read ends what can be trusted.
        auto stop = [&] {
            if (torn) *torn = true;
        };
        if (!v) {
            stop();
            break;
        }
        if (v->isArray()) {  // [at, status, data1, data2] (, route, voice)
            // As captured: a channel message (status 0x80..0xEF, 7-bit data),
            // from a device only if the take hears one, routed only if routed.
            const auto n = v->size();
            bool ok = !audio && ((n == 4 && c.midi.ports) || (n == 6 && c.midi.routed)) &&
                      clock(v->at(0), 0);
            CapturedMidiEvent e;
            for (std::size_t b = 0; ok && b < 3; ++b) {
                const std::int64_t low = b == 0 ? 0x80 : 0, high = b == 0 ? 0xef : 0x7f;
                ok = v->at(b + 1).isInt() && v->at(b + 1).asInt() >= low &&
                     v->at(b + 1).asInt() <= high;
                if (ok) e.bytes[b] = static_cast<std::uint8_t>(v->at(b + 1).asInt());
            }
            if (ok && n == 6) {
                const auto route =
                    v->at(4).isString() ? model::Id::parse(v->at(4).asString()) : std::nullopt;
                ok = route && route->kind == model::Kind::Route && v->at(5).isInt() &&
                     v->at(5).asInt() >= 0 && v->at(5).asInt() <= INT32_MAX;
                if (ok) {
                    e.route = *route;
                    e.voice = static_cast<int>(v->at(5).asInt());
                }
            }
            if (!ok) {
                stop();
                break;
            }
            e.at = v->at(0).asInt();
            c.events.push_back(e);
        } else if (const auto* run = v->isObject() ? v->find("run") : nullptr) {
            if (!run->isArray() || run->size() != 2 || !clock(run->at(0), 0) ||
                !clock(run->at(1)) ||
                (!c.runs.empty() && run->at(0).asInt() < c.runs.back().fileFrame)) {
                stop();
                break;
            }
            c.runs.push_back({run->at(0).asInt(), run->at(1).asInt()});
        } else if (const auto* captured = v->isObject() ? v->find("captured") : nullptr) {
            const auto* frames = v->find("frames");
            if (!clock(*captured, 0) || (punchAware && captured->asInt() < c.captured) ||
                (audio && (!frames || !clock(*frames, 0) || frames->asInt() < c.audioFrames ||
                           captured->asInt() < c.captured))) {
                stop();
                break;
            }
            if (const auto* value = v->find("punch"); punchAware && value) {
                auto punch = parsePunch(value);
                if (!punch) {
                    stop();
                    break;
                }
                c.punch = *punch;
            }
            c.captured = std::max(c.captured, captured->asInt());
            if (audio) c.audioFrames = frames->asInt();
        } else {
            stop();
            break;
        }
    }
    if (!header) return invalid("/0", "the journal has no header");
    std::stable_sort(c.events.begin(), c.events.end(),
                     [](const auto& a, const auto& b) { return a.at < b.at; });
    return c;
}

Result<MidiCapture> readMidiJournal(const std::filesystem::path& file,
                                    const std::filesystem::path& root, bool* torn) {
    return readJournal(file, root, torn, false);
}
Result<RecordingInfo> readAudioJournal(const std::filesystem::path& wav,
                                       const std::filesystem::path& root, bool* torn) {
    auto capture = readJournal(audioJournalPath(wav), root, torn, true);
    if (!capture) return std::move(capture).error();
    const auto& c = *capture;
    RecordingInfo info{c.track, c.start, c.rate, {}, c.loop.start, c.loop.end};
    info.frames = c.audioFrames;
    info.punch = c.punch;
    if (c.audioFrames > 0) info.segments.push_back({0, c.start});
    for (const auto& run : c.runs) {
        const auto frame = run.fileFrame + c.latency;
        if (frame < c.audioFrames) info.segments.push_back({frame, run.timeline});
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

Recorder::Recorder(std::vector<RecordTarget> targets, double rate, std::int64_t latency,
                   RecordedPunch punch)
    : rate_(rate), latency_(latency), startPunch_(punch), punch_(punch) {
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

Status Recorder::setPunch(RecordedPunch punch) {
    SUPPLE_TRY(validateRecordingPunch(punch));
    std::lock_guard lock(punchMutex_);
    punch_ = punch;
    return okStatus();
}
RecordedPunch Recorder::getPunch() {
    std::lock_guard lock(punchMutex_);
    return punch_;
}

Recorder::~Recorder() {
    stop_ = true;
    if (writer_.joinable()) writer_.join();
}

Status Recorder::open() {
    SUPPLE_TRY(validateRecordingPunch(startPunch_));
    std::promise<Status> opened;
    auto ready = opened.get_future();
    writer_ = std::thread([this, opened = std::move(opened)]() mutable {
        auto status = openFiles();
        if (!status) closeFiles();
        const bool run = status.ok();
        opened.set_value(std::move(status));
        if (run) {
            writerLoop();
            closeFiles();
        }
    });
    return ready.get();
}

Status Recorder::openFiles() {
    for (auto& t : tracks_) {
        std::error_code ec;
        std::filesystem::create_directories(t->target.file.parent_path(), ec);
        SUPPLE_TRY(t->writer.open(t->target.file, rate_, t->ring.channels()));
    }
    for (auto& t : tracks_) {
        openJournal(*t, audioJournalPath(t->target.file));
        rememberError(*t, t->journalError);
    }
    // MIDI keeps its warning/in-memory fallback if the journal cannot open.
    for (auto& t : midiTracks_) {
        if (t->target.file.empty()) continue;
        std::error_code ec;
        std::filesystem::create_directories(t->target.file.parent_path(), ec);
        openJournal(*t, t->target.file);
    }
    updateHeaders();  // establish the initial audio file and namespace checkpoint
    return okStatus();
}

void Recorder::closeFiles() {
    for (auto& t : tracks_) {
        const bool opened = t->writer.isOpen();
        rememberError(*t, t->writer.close());
        if (opened) rememberError(*t, syncParentDirectories(t->target.file));
    }
    closeJournals();
}

std::vector<RecordingError> Recorder::takeErrors() {
    std::vector<RecordingError> errors;
    for (auto& t : tracks_)
        if (t->errorPending.exchange(false, std::memory_order_acquire))
            errors.push_back({t->target.track, t->target.file, t->firstError.error()});
    return errors;
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
    published_.store(captured_, std::memory_order_release);
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

void Recorder::openJournal(Journal& t, const std::filesystem::path& path) {
    t.journalFile = path;
    std::error_code why;
    t.journal = files::createAppend(path, why);
    t.journalCreated = t.journal != nullptr;
    if (!t.journal)
        t.journalError =
            Error("record.journal", "cannot create " + path.string() + ": " + why.message(),
                  "/recording", "make the recordings folder writable; retained audio is kept");
}

void Recorder::appendJournal(Journal& t, const RecordTarget& target,
                             std::span<const MidiEvent> events, std::int64_t frames,
                             RecordedPunch punch) {
    const auto captured = published_.load(std::memory_order_acquire);
    const auto runs = segmentCount_.load(std::memory_order_acquire);
    const bool audio = !target.midi;
    std::string out;
    if (!t.header) {
        json::Value h = json::Value::object();
        h["schema"] = audio ? "supple.audio-recording/1.1" : std::string(kJournalSchema);
        h["punch"] = punchJson(startPunch_);
        h["track"] = target.track.str();
        h["sample_rate"] = rate_;
        h["start_sample"] = static_cast<long long>(start_.load(std::memory_order_relaxed));
        h["latency"] = static_cast<long long>(latency_);
        if (audio)
            h["channels"] = target.output || target.rackInput
                                ? 2
                                : static_cast<int>(std::max<std::size_t>(1, target.inputs.size()));
        else {
            h["device"] = target.midi->ports != 0;
            h["routed"] = target.midi->routed;
            h["shift"] = static_cast<long long>(target.midi->shift);
        }
        json::Value loop = json::Value::array();
        const LoopRange l = startLoop_.active() ? startLoop_ : LoopRange{};
        loop.push_back(static_cast<long long>(l.start));
        loop.push_back(static_cast<long long>(l.end));
        h["loop"] = std::move(loop);
        out += journalLine(h);
        t.header = true;
    }
    for (; t.runsJournaled < runs; ++t.runsJournaled) {
        const auto& sg = segments_[t.runsJournaled];
        json::Value row = json::Value::array();
        row.push_back(static_cast<long long>(sg.fileFrame));
        row.push_back(static_cast<long long>(sg.timeline));
        json::Value line = json::Value::object();
        line["run"] = std::move(row);
        out += journalLine(line);
    }
    for (; t.eventsJournaled < events.size(); ++t.eventsJournaled) {
        const auto& e = events[t.eventsJournaled];
        json::Value row = json::Value::array();
        row.push_back(static_cast<long long>(e.at));
        for (auto byte : e.bytes) row.push_back(static_cast<long long>(byte));
        if (e.route != model::Id{}) {
            row.push_back(e.route.str());
            row.push_back(static_cast<long long>(e.voice));
        }
        out += journalLine(row);
    }
    if (captured != t.capturedJournaled || (audio && frames != t.framesJournaled) ||
        t.punchJournaled != punch) {
        json::Value line = json::Value::object();
        line["captured"] = static_cast<long long>(captured);
        if (audio) line["frames"] = static_cast<long long>(frames);
        if (t.punchJournaled != punch) line["punch"] = punchJson(punch);
        out += journalLine(line);
        t.capturedJournaled = captured;
        t.framesJournaled = frames;
        t.punchJournaled = punch;
    }
    if (!out.empty() && std::fwrite(out.data(), 1, out.size(), t.journal) != out.size())
        t.journalError = Error("record.journal", "cannot write " + t.journalFile.string(),
                               "/recording", "free disk space; retained audio is kept");
}

void Recorder::writeJournals(bool sync) {
    if (!started_.load(std::memory_order_acquire)) return;
    const auto punch = getPunch();
    auto write = [&](Journal& t, const RecordTarget& target, std::span<const MidiEvent> events,
                     std::int64_t frames) {
        if (!t.journal || !t.journalError) return;
        appendJournal(t, target, events, frames, punch);
        if (t.journalError && (sync ? !files::syncFile(t.journal) : std::fflush(t.journal) != 0))
            t.journalError =
                Error("record.journal", "cannot flush " + t.journalFile.string(), "/recording",
                      "check storage; this take has no new crash checkpoint");
        if (sync && t.journalError) {
            auto directory = syncParentDirectories(t.journalFile);
            if (!directory)
                t.journalError =
                    Error("record.journal", directory.error().message, "/recording",
                          "check storage; this take has no new durable crash checkpoint");
        }
    };
    for (auto& t : tracks_) {
        // Never publish progress whose complete audio bytes have not reached the OS.
        // A failed data checkpoint must not advance the trusted placement prefix.
        if (!t->writeError) continue;
        auto flushed = t->writer.flush();
        rememberError(*t, flushed);
        if (!flushed) continue;
        write(*t, t->target, {}, static_cast<std::int64_t>(t->writer.frames()));
        rememberError(*t, t->journalError);
    }
    for (auto& t : midiTracks_) write(*t, t->target, t->drained, 0);
}

void Recorder::closeJournals() {
    auto close = [](Journal& t) {
        if (t.journal) {
            const bool failed = std::fclose(t.journal) != 0;
            t.journal = nullptr;
            if (failed && t.journalError)
                t.journalError = Error("record.journal", "cannot close " + t.journalFile.string(),
                                       "/recording", "check storage; retained audio is kept");
        }
    };
    for (auto& t : tracks_) {
        close(*t);
        rememberError(*t, t->journalError);
    }
    for (auto& t : midiTracks_) close(*t);
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
        const auto before = t.writer.frames();
        if (t.writeError) {
            auto st = t.writer.append(scratch.data(), n);
            rememberError(t, std::move(st));
        }
        if (!t.writeError) {
            const auto expected = before + n;
            const auto retained = t.writer.frames();
            if (expected > retained)
                t.overflow.fetch_add(expected - retained, std::memory_order_relaxed);
        }
    }
    if (t.writeError) {
        const auto before = t.writer.frames();
        auto st = t.writer.flush();  // a crash of the process keeps what the OS has
        rememberError(t, std::move(st));
        if (before > t.writer.frames())
            t.overflow.fetch_add(before - t.writer.frames(), std::memory_order_relaxed);
    }
}

void Recorder::writeSidecars() {
    if (sidecars_ || !started_.load(std::memory_order_acquire)) return;
    sidecars_ = true;
    const std::int64_t start = start_.load(std::memory_order_relaxed);
    for (auto& t : tracks_) {
        // Without one the take is fine but a crash would leave it unplaced.
        RecordingInfo info{t->target.track, start, rate_, {}, 0, 0};
        info.punch = startPunch_;
        auto st = writeSidecar(t->target.file, info);
        rememberError(*t, std::move(st), true);
    }
}

void Recorder::rememberError(Track& track, Status status, bool sidecar) {
    if (status) return;
    if (track.firstError) {
        track.firstError = status.error();
        track.errorPending.store(true, std::memory_order_release);
        track.errorPending.notify_all();
        if (mainWake_) mainWake_->notify();
    }
    Status& target = sidecar ? track.sidecarError : track.writeError;
    if (target) target = std::move(status);
    if (sidecar && track.writeError) track.writeError = track.sidecarError.error();
}

void Recorder::updateHeaders() {
    for (auto& t : tracks_)
        if (t->writeError) {
            rememberError(*t, t->writer.sync());
            if (t->writeError) rememberError(*t, syncParentDirectories(t->target.file));
        }
}

void Recorder::writerLoop() {
    nameThread("audio-recorder");
    (void)diagctx::registerThread(diagctx::ThreadRole::Recorder);
    std::vector<float> scratch(8192 * 16);
    auto now = [&] {
        return nowForTests_ ? nowForTests_(clockContext_) : std::chrono::steady_clock::now();
    };
    auto checkpoint = now() + std::chrono::seconds(5);
    while (!stop_.load(std::memory_order_acquire)) {
        if (writerStalled_.load(std::memory_order_acquire)) {
            writerStallAck_.store(true, std::memory_order_release);
            writerStallAck_.notify_all();
            while (writerStalled_.load(std::memory_order_acquire) && !stop_.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            writerStallAck_.store(false, std::memory_order_release);
        }
        writeSidecars();
        for (auto& t : tracks_) drain(*t, scratch);
        drainMidi();  // the MIDI rings too (bounded: a long performance must not overflow)
        const auto at = now();
        const bool due = at >= checkpoint;
        if (due) {
            updateHeaders();  // data/header before the journals that describe them
            checkpoint = at + std::chrono::seconds(5);
        }
        writeJournals(due);
        writerPasses_.fetch_add(1, std::memory_order_release);
        writerPasses_.notify_all();
        std::this_thread::sleep_for(std::min(
            checkpoint - now(), std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                    std::chrono::milliseconds(20))));
    }
    writeSidecars();
    drainMidi();
    for (auto& t : tracks_) drain(*t, scratch);    // what arrived before the stop
    for (auto& t : tracks_) padTo(*t, captured_);  // a final dropped tail has no next timestamp
    updateHeaders();
    writeJournals(true);
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
            info.frames = static_cast<std::int64_t>(t->writer.frames());
            info.punch = getPunch();
            rememberError(*t, writeSidecar(t->target.file, info), true);
        }
    }
}

std::vector<RecordedTake> Recorder::finish(LoopRange loop) {
    // Once configured, the initial loop is authoritative (ADR-0080/0082).
    if (loopConfigured_) loop = startLoop_;
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
    const auto punch = getPunch();
    // MIDI (ADR-0016): placed by the rule a journal's recovery uses too.
    for (auto& t : midiTracks_) {
        MidiCapture c;
        c.track = t->target.track;
        c.rate = rate_;
        c.start = start;
        c.latency = latency_;
        c.midi = *t->target.midi;
        c.runs.assign(segments_.begin(), segments_.begin() + static_cast<std::ptrdiff_t>(count));
        c.captured = captured_;
        c.loop = loop;
        c.events = t->drained;
        c.overflow = t->overflow.load();
        c.runsFull = runsFull_;
        c.punch = punch;
        RecordedTake r = placeMidi(c);
        // The journal, only if this recording created it: a file already at
        // its name (refused by the exclusive creation) is someone else's,
        // and the app deletes a take's journal once a save holds the take.
        if (t->journalCreated) r.file = t->target.file;
        if (!t->journalError) r.warnings.push_back(t->journalError.error());
        out.push_back(std::move(r));
    }
    for (auto& t : tracks_) {
        RecordedTake r;
        r.track = t->target.track;
        r.file = t->target.file;
        r.punch = punch;
        if (t->journalCreated) r.journal = t->journalFile;
        r.startSample = start;
        r.sampleRate = rate_;
        r.frames = static_cast<std::int64_t>(t->writer.frames());
        for (const RecordedSegment& sg : segments)
            if (sg.fileFrame < r.frames) r.segments.push_back(sg);
        r.loop = {loop.start, loop.end};
        r.overflowFrames = t->overflow.load();
        if (!t->firstError) {
            r.failure = t->firstError.error();
            r.error = r.failure->toString();
        }
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
