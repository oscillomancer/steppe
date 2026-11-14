// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Internal: recording (ADR-0014 §1–2). The audio callback copies each armed
// track's input channels into an SPSC frame ring; a writer thread drains the
// rings into WAV files, rewriting their headers every few seconds so a crash
// leaves readable takes. The callback never blocks; overflow is counted.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <thread>
#include <vector>

#include "supple/audio/wav_stream.h"
#include "supple/base/rt.h"
#include "supple/engine/realtime.h"
#include "supple/json/json.h"

namespace supple::engine {
struct TransportRange;
}

namespace supple::engine::detail {

// Interleaved float frames, one producer (the audio callback), one consumer.
class FrameRing {
  public:
    void init(std::uint32_t channels, std::size_t frames);
    // Copies `frames` of the selected input channels (missing ones: silence);
    // returns how many fitted.
    std::size_t write(const float* const* in, std::uint32_t inChannels, std::span<const int> select,
                      std::uint32_t frames, std::int64_t firstFrame = 0) SUPPLE_RT;
    // With firstFrame, read only one contiguous file-time run.
    std::size_t read(float* out, std::size_t maxFrames, std::int64_t* firstFrame = nullptr);
    std::uint32_t channels() const { return channels_; }

  private:
    std::vector<float> buf_;
    std::vector<std::int64_t> positions_;
    std::size_t capacity_ = 0;  // frames
    std::uint32_t channels_ = 0;
    alignas(64) std::atomic<std::size_t> write_{0};
    alignas(64) std::atomic<std::size_t> read_{0};
};

// A recording's sidecar (ADR-0014 §3), next to its WAV file: where the take
// belongs. Written as soon as capture starts, so a crash before the stop
// commits the take still leaves enough to place it (project.recover).
struct RecordingInfo {
    model::Id track;
    std::int64_t startSample = 0;  // timeline sample of the file's first frame
    double sampleRate = 0;
    // Written when the recording ends (loop passes, ADR-0015); empty before.
    std::vector<RecordedSegment> segments;
    std::int64_t loopStart = 0, loopEnd = 0;
    std::string hash = {};  // optional; finalized by the background recording job
};
// A track's input after its input FX (ADR-0014): what it records
// instead of the raw device channels, and the latency of those FX. For a
// recorded master (ADR-0057), its output before its own items; the recorder
// places it from the target's outputShift and ignores `latency`. A rack Out
// uses its channel's pre-FX tap, with outputShift including its input FX.
struct ProcessedInput {
    model::Id track;
    const float* channels[2] = {nullptr, nullptr};
    std::int64_t latency = 0;
};

struct RoutedMidiEvent {
    std::uint32_t frame = 0;
    std::uint8_t bytes[3] = {0, 0, 0};
    model::Id route = {};
    int voice = -1;
};

// A MIDI message as captured (ADR-0016): `at` counts captured frames, as an
// audio file's frames do; the runs place it on the timeline.
struct CapturedMidiEvent {
    std::int64_t at = 0;
    std::uint8_t bytes[3] = {0, 0, 0};
    model::Id route = {};
    int voice = -1;
};

// What placing a MIDI take needs. The recorder has it when recording stops;
// a crash leaves it in the take's journal (ADR-0080).
struct MidiCapture {
    model::Id track;
    double rate = 0;
    std::int64_t start = 0;    // timeline sample of captured frame 0, less the device round trip
    std::int64_t latency = 0;  // the device round trip
    RecordTarget::Midi midi;   // ports: only whether there are any
    std::vector<RecordedSegment> runs;  // after the first: {captured frame, timeline}
    std::int64_t captured = 0;          // frames captured
    LoopRange loop;                     // inactive: runs split by seeks
    std::vector<CapturedMidiEvent> events;
    std::uint64_t overflow = 0;
    bool runsFull = false;
};
// The clock a MIDI target's events are captured on: the device round trip,
// or a Note Out destination's input shift (ADR-0016).
std::int64_t midiClock(const RecordTarget::Midi& midi, std::int64_t latency);
// The take stop commits, from what was captured (events in time order).
RecordedTake placeMidi(const MidiCapture& capture);

// A MIDI take's crash-recovery journal (ADR-0080), <name>.midi.jsonl beside
// the WAVs: a header line, then events, runs and how far capture got, one
// canonical JSON value per line, appended by the recorder's writer thread.
bool isMidiJournal(const std::filesystem::path& file);
// Reads one, only inside `root` (media_files.h). A torn last line (the
// write a crash interrupted) is left out and `torn` set; so is everything
// from the first line that does not read. record.journal when the header
// does not read.
Result<MidiCapture> readMidiJournal(const std::filesystem::path& file,
                                    const std::filesystem::path& root, bool* torn = nullptr);

// record.commit's arguments for takes committed with `punch` (take_commands.cpp):
// files relative to `media` when inside it, so they resolve after a save
// moves the project (ADR-0076).
json::Value recordCommitArgs(const std::vector<RecordedTake>& takes, const TransportRange& punch,
                             const std::filesystem::path& media);

std::filesystem::path sidecarPath(const std::filesystem::path& wav);
Status writeSidecar(const std::filesystem::path& wav, const RecordingInfo& info);
// `root` (optional): the sidecar is read only if it resolves inside it
// (recover.outside otherwise; media_files.h).
Result<RecordingInfo> readSidecar(const std::filesystem::path& wav,
                                  const std::filesystem::path& root = {});

class Recorder {
  public:
    Recorder(std::vector<RecordTarget> targets, double rate, std::int64_t latency);
    ~Recorder();
    // While slaved (ADR-0071) the timeline moves at the chase rate, not one
    // frame per frame: a position within a block of the expected one is the
    // same run (the take drifts by the chase rate's distance from 1); only a
    // larger jump (a locate, a loop wrap) starts a new one. Before capture.
    void setDriftTolerant(bool on) { driftTolerant_ = on; }
    // The loop in effect when recording starts, for the MIDI journals'
    // headers (ADR-0080). Before open().
    void setLoop(LoopRange loop) { startLoop_ = loop; }
    // Creates the files (WAVs, and the MIDI targets' journals at their
    // `file`) and starts the writer thread.
    Status open();
    // Audio thread: `pos` is the timeline sample of this callback's output.
    // A track listed in `processed` records that signal, its first `latency`
    // frames dropped so the file lines up with what was played.
    void capture(const float* const* in, std::uint32_t inChannels, std::uint32_t frames,
                 std::int64_t pos, std::span<const ProcessedInput> processed = {}) SUPPLE_RT;
    // Audio thread, after capture() for the same part of the cycle: the
    // device MIDI in frames [offset, offset + frames) of `events` (ADR-0016).
    void captureMidi(std::span<const MidiInputEvent> events, std::uint32_t offset,
                     std::uint32_t frames, std::int64_t pos) SUPPLE_RT;
    void captureRoutedMidi(model::Id track, std::span<const RoutedMidiEvent> events,
                           std::uint32_t frames) SUPPLE_RT;
    // The consumer of the MIDI rings: the writer thread while recording, then
    // finish() after joining it.
    void drainMidi();
    // Audio thread: input that could not be placed (a playback dropout).
    void lose(std::uint32_t frames) SUPPLE_RT;
    // Main thread, once capture() can no longer run: drains, closes, reports
    // (`loop`: the loop in effect, for takes per pass).
    std::vector<RecordedTake> finish(LoopRange loop = {});

  private:
    friend struct RecorderTestAccess;
    struct Track {
        RecordTarget target;
        FrameRing ring;
        audio::WavStreamWriter writer;
        std::atomic<std::uint64_t> overflow{0};
        Status writeError = okStatus();  // writer thread, then finish after join
        Status sidecarError = okStatus();
        Status firstError = okStatus();
        // Input FX latency (set at the first frame), or a master's offset
        // (set at construction, ADR-0057): `skip` frames still to drop,
        // `shift` how far the file runs behind the captured frames (ahead of
        // them when negative: the start is padded with silence).
        std::int64_t skip = 0, shift = 0;
    };
    // MIDI: events stamped with the captured-frame position they arrived at
    // (placed on the timeline by finish(), knowing the runs).
    using MidiEvent = CapturedMidiEvent;
    struct MidiTrack {
        RecordTarget target;
        static constexpr std::size_t kRing = 4096;
        std::array<MidiEvent, kRing> ring{};
        alignas(64) std::atomic<std::size_t> write{0};
        alignas(64) std::atomic<std::size_t> read{0};
        std::vector<MidiEvent> drained;  // the writer thread, then finish()
        std::atomic<std::uint64_t> overflow{0};
        // The journal (ADR-0080), the writer thread's, then finish()'s.
        std::FILE* journal = nullptr;
        Status journalError = okStatus();
        bool header = false;
        std::size_t eventsJournaled = 0, runsJournaled = 0;
        std::int64_t capturedJournaled = -1;
    };
    void writerLoop();
    std::int64_t midiClock(const MidiTrack& track) const SUPPLE_RT;
    void pushMidi(MidiTrack& track, const MidiEvent& event) SUPPLE_RT;
    // Appends what the MIDI tracks drained since the last call to their
    // journals and flushes them to the OS; `sync`: to the disk too.
    void writeJournals(bool sync);
    void closeJournals();
    void drain(Track& t, std::vector<float>& scratch);
    void padTo(Track& t, std::int64_t frame);
    void writeSidecars();
    void updateHeaders();
    static void rememberError(Track& track, Status status, bool sidecar = false);

    std::vector<std::unique_ptr<Track>> tracks_;
    std::vector<std::unique_ptr<MidiTrack>> midiTracks_;
    double rate_;
    std::int64_t latency_;
    std::atomic<bool> started_{false};
    std::atomic<std::int64_t> start_{0};
    // Audio thread: where the next captured frame would continue the run,
    // frames captured so far, and the runs after the first (preallocated;
    // read by finish() after capture has stopped).
    std::int64_t expected_ = 0, captured_ = 0;
    // captured_ as of the end of the latest capture(), for the journals: a
    // MIDI event the writer has drained was captured no later than this.
    std::atomic<std::int64_t> published_{0};
    LoopRange startLoop_;
    bool driftTolerant_ = false;
    bool runsFull_ = false;
    static constexpr std::size_t kMaxSegments = 4096;
    std::array<RecordedSegment, kMaxSegments> segments_{};
    std::atomic<std::size_t> segmentCount_{0};
    LoopRange finishingLoop_;  // published to the writer by stop_ (capture already stopped)
    std::atomic<bool> stop_{false};
    std::atomic<bool> writerStalled_{false}, writerStallAck_{false};  // test access
    std::thread writer_;
    bool sidecars_ = false;  // writer thread
};

}  // namespace supple::engine::detail
