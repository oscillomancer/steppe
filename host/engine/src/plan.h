// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Internal: the compiled render plan shared by the offline renderer and the
// real-time engine (engine.md §1–2). Not a public interface.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include "playrate_timeline.h"
#include "recording.h"
#include "strip.h"
#include "supple/dsp/oversampler.h"
#include "supple/engine/offline.h"
#include "supple/engine/realtime.h"
#include "supple/model/env_shape.h"
#include "supple/params/params.h"
#include "supple/time/timeline.h"
#include "workers.h"

namespace supple::engine::detail {

using model::Id;
using model::Kind;
using model::Session;

inline double dbToGain(double db) {
    return db <= model::kSilenceDb ? 0.0 : std::pow(10.0, db / 20.0);
}

// A fixed-length delay for one stereo signal (allocated at compile time).
struct Delay {
    std::int64_t length = 0;
    std::vector<float> buf[2];
    std::size_t pos = 0;
    void init(std::int64_t n) {
        length = n;
        for (auto& b : buf) b.assign(static_cast<std::size_t>(std::max<std::int64_t>(n, 1)), 0.0f);
        pos = 0;
    }
    void clear() SUPPLE_RT {
        for (auto& b : buf) std::fill(b.begin(), b.end(), 0.0f);
        pos = 0;
    }
    // In place on `frames` samples of L/R.
    void process(float* l, float* r, std::uint32_t frames) SUPPLE_RT {
        if (length == 0) return;
        const auto n = static_cast<std::size_t>(length);
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float dl = buf[0][pos], dr = buf[1][pos];
            buf[0][pos] = l[i];
            buf[1][pos] = r[i];
            l[i] = dl;
            r[i] = dr;
            pos = pos + 1 == n ? 0 : pos + 1;
        }
    }
};

// A fixed-length delay for one channel (an auxiliary channel, ADR-0020).
struct MonoDelay {
    std::vector<float> buf;
    std::size_t pos = 0;
    std::int64_t length = 0;
    void init(std::int64_t n) {
        length = n;
        buf.assign(static_cast<std::size_t>(std::max<std::int64_t>(n, 1)), 0.0f);
        pos = 0;
    }
    void clear() SUPPLE_RT {
        std::fill(buf.begin(), buf.end(), 0.0f);
        pos = 0;
    }
    void process(float* x, std::uint32_t frames) SUPPLE_RT {
        if (length == 0) return;
        const auto n = static_cast<std::size_t>(length);
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float d = buf[pos];
            buf[pos] = x[i];
            x[i] = d;
            pos = pos + 1 == n ? 0 : pos + 1;
        }
    }
};

struct Stereo {
    std::vector<float> l, r;
    void init(std::size_t n) {
        l.assign(n, 0.0f);
        r.assign(n, 0.0f);
    }
    void clear(std::uint32_t n) SUPPLE_RT {
        std::fill_n(l.begin(), n, 0.0f);
        std::fill_n(r.begin(), n, 0.0f);
    }
    void copyFrom(const Stereo& o, std::uint32_t n) SUPPLE_RT {
        std::copy_n(o.l.begin(), n, l.begin());
        std::copy_n(o.r.begin(), n, r.begin());
    }
};

using model::shapeCurve;  // supple/model/env_shape.h: shared with the GUI

// A compiled envelope: points at samples from the render start (already
// shifted by the latency at the point of application), values in the
// target's unit. Playback is monotonic, so a cursor makes lookups O(1).
struct Automation {
    struct Pt {
        std::int64_t sample;
        double value;
        model::PointShape shape;
        double tension;
    };
    std::vector<Pt> points;
    std::size_t cursor = 0;
    PlayrateTimeline* timeline = nullptr;
    std::int64_t deviceShift = 0;
    bool active() const { return !points.empty(); }
    void shift(std::int64_t by) {
        deviceShift += by;
        for (auto& pt : points) pt.sample += by;
    }
    double at(std::int64_t device) SUPPLE_RT {
        const double s =
            timeline ? timeline->absolutePosition(device - deviceShift) + double(deviceShift)
                     : double(device);
        const double origin = timeline ? timeline->origin() : 0;
        if (s <= double(points.front().sample) + origin) return points.front().value;
        if (s < double(points[cursor].sample) + origin) cursor = 0;
        while (cursor + 1 < points.size() && double(points[cursor + 1].sample) + origin <= s)
            ++cursor;
        if (cursor + 1 >= points.size()) return points.back().value;
        const Pt& a = points[cursor];
        const Pt& b = points[cursor + 1];
        const double t =
            (s - (double(a.sample) + origin)) / static_cast<double>(b.sample - a.sample);
        return a.value + (b.value - a.value) * shapeCurve(a.shape, a.tension, t);
    }
};

// Per-consumer VCA curves: each cursor belongs to exactly one executor/node.
struct VcaControl {
    double volumeDb = 0;
    bool mute = false;
    Automation volume, gate;
    std::uint32_t slot = 0;  // into MixSlots::vcas
    bool hold = false;       // a write pass holds the level (ADR-0061)
    bool curve() const SUPPLE_RT { return volume.active() && !hold; }
};
struct VcaControls {
    std::vector<VcaControl> values;
    bool empty() const SUPPLE_RT { return values.empty(); }
    bool automated() const SUPPLE_RT {
        for (const auto& v : values)
            if (v.curve()) return true;
        return false;
    }
    void evaluate(std::int64_t at, double& db, bool& muted) SUPPLE_RT {
        bool silence = db <= model::kSilenceDb;
        for (auto& v : values) {
            const double value = v.curve() ? v.volume.at(at) : v.volumeDb;
            silence |= value <= model::kSilenceDb;
            db += value;
            muted |= v.gate.active() ? v.gate.at(at) >= .5 : v.mute;
        }
        if (silence) db = model::kSilenceDb;
    }
    void shift(std::int64_t at) {
        for (auto& v : values) {
            v.volume.shift(at);
            v.gate.shift(at);
        }
    }
    void timeline(PlayrateTimeline* clock) {
        for (auto& v : values) {
            v.volume.timeline = clock;
            v.gate.timeline = clock;
        }
    }
    void reset() SUPPLE_RT {
        for (auto& v : values) {
            v.volume.cursor = 0;
            v.gate.cursor = 0;
        }
    }
};

// One render block of a signal (the master's, or a frontier edge's tap),
// tagged with the transport epoch it belongs to (ADR-0012, ADR-0014).
struct Chunk {
    std::uint64_t epoch = 0;
    std::int64_t start = 0;   // plan (timeline) sample of the first frame
    std::int64_t stream = 0;  // frames rendered in this epoch before it (ADR-0015)
    std::uint32_t frames = 0;
    std::vector<float> l, r;
    // The root ring only: device output channels 3 and up (ADR-0056).
    std::vector<std::vector<float>> extra;
    std::vector<double> timeline, rates;  // root ring only, per consumed boundary
    std::uint32_t width() const SUPPLE_RT { return 2 + static_cast<std::uint32_t>(extra.size()); }
    float* channel(std::uint32_t c) SUPPLE_RT {
        return c == 0 ? l.data() : c == 1 ? r.data() : extra[c - 2].data();
    }
    const float* channel(std::uint32_t c) const SUPPLE_RT {
        return c == 0 ? l.data() : c == 1 ? r.data() : extra[c - 2].data();
    }
};

// Single producer (the renderer), single consumer (the audio callback).
struct ChunkRing {
    std::vector<Chunk> chunks;
    std::size_t mask = 0;
    alignas(64) std::atomic<std::size_t> write{0};
    alignas(64) std::atomic<std::size_t> read{0};

    // `channels`: 2, or the device's outputs for the root ring.
    ChunkRing(std::size_t capacity, std::uint32_t frames, bool metadata = false,
              std::uint32_t channels = 2) {
        chunks.resize(std::bit_ceil(std::max<std::size_t>(capacity, 4)));
        mask = chunks.size() - 1;
        for (Chunk& c : chunks) {
            c.l.assign(frames, 0.0f);
            c.r.assign(frames, 0.0f);
            c.extra.assign(channels > 2 ? channels - 2 : 0, std::vector<float>(frames, 0.0f));
            if (metadata) {
                c.timeline.resize(frames + 1);
                c.rates.resize(frames + 1, 1.);
            }
        }
    }
    // Producer: the next free chunk, or null when the ring is full.
    Chunk* slot() {
        const std::size_t w = write.load(std::memory_order_relaxed);
        return w - read.load(std::memory_order_acquire) > mask ? nullptr : &chunks[w & mask];
    }
    void publish() {
        write.store(write.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }
    // Consumer.
    const Chunk* front() const SUPPLE_RT {
        const std::size_t r = read.load(std::memory_order_relaxed);
        return r == write.load(std::memory_order_acquire) ? nullptr : &chunks[r & mask];
    }
    void pop() SUPPLE_RT {
        read.store(read.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }
    // Consumer: drops the chunks of epochs before `e` (flushed by a seek).
    void dropBefore(std::uint64_t e) SUPPLE_RT {
        while (const Chunk* c = front()) {
            if (c->epoch >= e) break;
            pop();
        }
    }
    // Consumer: frames [stream, stream + n) of epoch `e` into l/r, dropping
    // older chunks; frames not rendered yet are zero. Returns the frames
    // found. Stream positions only grow, so a loop wrap needs nothing here.
    std::uint32_t pullAt(std::uint64_t e, std::int64_t stream, float* l, float* r,
                         std::uint32_t n) SUPPLE_RT {
        std::uint32_t found = 0;
        for (std::uint32_t done = 0; done < n;) {
            const Chunk* c = front();
            if (c && (c->epoch < e || (c->epoch == e && c->stream + c->frames <= stream + done))) {
                pop();  // flushed, or behind the play position
                continue;
            }
            if (!c || c->epoch != e || c->stream > stream + done) {
                std::fill_n(l + done, n - done, 0.0f);
                std::fill_n(r + done, n - done, 0.0f);
                break;
            }
            const auto at = static_cast<std::uint32_t>(stream + done - c->stream);
            const std::uint32_t take = std::min(n - done, c->frames - at);
            std::copy_n(c->l.begin() + at, take, l + done);
            std::copy_n(c->r.begin() + at, take, r + done);
            done += take;
            found += take;
            if (at + take == c->frames) pop();
        }
        return found;
    }
};

struct Edge {
    std::size_t from = 0;     // node index
    std::size_t to = 0;       // node index of the destination
    std::optional<Id> route;  // empty for parent sends
    // A key route (ADR-0018): the destination chain stage whose sidechain
    // input this feeds (-1: the node's input sum).
    int stage = -1;
    model::Tap tap = model::Tap::PostFader;
    double gainL = 1.0, gainR = 1.0;
    double volumeDb = 0.0, pan = 0.0;
    double staticGain = 1, staticPan = .5;
    bool mono = false, invert = false;
    // The route's channel map (session-model §1.8) as a matrix: destination
    // channel × source channel. Off: identity.
    bool mapped = false;
    float map[2][2] = {{1.0f, 0.0f}, {0.0f, 1.0f}};
    void applyMap(float& l, float& r) const SUPPLE_RT {
        if (!mapped) return;
        const float a = l, b = r;
        l = map[0][0] * a + map[0][1] * b;
        r = map[1][0] * a + map[1][1] * b;
    }
    Automation autoVolume, autoPan, autoMute;  // route envelopes (dB, %, 0/1)
    VcaControls vcas;
    Automation stripVolume, stripPan, stripMute;  // private copies: source-owned
    bool encode = false, followPan = true, muted = false;
    // A write pass holds the route's or its source strip's level or pan at
    // the static value instead of the compiled curve (ADR-0061).
    bool holdVolume = false, holdPan = false, holdStripVolume = false, holdStripPan = false;
    bool volumeCurve() const SUPPLE_RT { return autoVolume.active() && !holdVolume; }
    bool panCurve() const SUPPLE_RT { return autoPan.active() && !holdPan; }
    bool stripVolumeCurve() const SUPPLE_RT { return stripVolume.active() && !holdStripVolume; }
    bool stripPanCurve() const SUPPLE_RT { return stripPan.active() && !holdStripPan; }
    ConsoleStage<console9::Channel> encoder;
    FaderControl fader;
    Stereo encoded;
    Delay delay;
    bool preserveDelayOnLocate = false;
    Stereo tmp;
    // Channel pairs that touch a strip's auxiliary channels (ADR-0020, 0-based
    // channels: 0/1 the main pair): linear, at the route's level, mute and
    // polarity (its envelopes act on the main pair). A source's auxiliary
    // channel is read after its chain; a main channel at the route's tap
    // (post fader: before the encode, at the strip's level). `auxOnly`: the
    // route maps nothing between main pairs.
    struct AuxLink {
        int from = 0, to = 0;
        MonoDelay delay;
        bool preserveDelayOnLocate = false;
    };
    std::vector<AuxLink> auxLinks;
    bool auxOnly = false;
    std::vector<float> auxTmp;
    // A frontier edge (anticipative source, live target): its source's tap
    // arrives through a ring, and the callback copies it here for the target.
    bool frontier = false;
    std::size_t ring = 0;  // index into Plan::frontierRings
    Stereo frontierBuf;
};

// An automated plugin parameter (engine.md §1.4, ADR-0010): semantic
// parameters interpolate in the normalized (curve) domain from physical
// points; plain CLAP parameters in their CLAP value. Values reach the plugin
// as param events at every point and every kAutomationStep samples.
inline constexpr std::uint32_t kAutomationStep = 32;
struct ParamLane {
    std::uint32_t clapId = 0;
    Automation curve;
    std::shared_ptr<const params::DescribedParam> spec;  // null: plain CLAP values
    double lastSent = std::numeric_limits<double>::quiet_NaN();
    double clapAt(std::int64_t sample) SUPPLE_RT {
        const double v = curve.at(sample);
        return spec ? params::toClap(spec->spec, params::fromNormalized(spec->spec, v)) : v;
    }
};

// Held input notes (not release tails). Preserve the full CLAP address: a
// NoteEnd with id -1 uses the tuple, and each -1 field is a wildcard.
struct NoteOrigin {
    std::uint64_t item = 0, take = 0, source = 0;
    std::int64_t tick = 0;
    std::int32_t pass = 0;
    auto address() const { return std::tuple{item, take, source, tick, pass}; }
};
struct NoteSource {
    NoteOrigin origin{};
    std::int64_t start = 0, end = 0;
    std::int16_t port = 0, channel = 0, key = 0;
    std::uint32_t ordinal = 0;
    std::int32_t id = -1;
    auto addressWithoutOrdinal() const {
        return std::tuple_cat(origin.address(), std::tuple{port, channel, key});
    }
    auto address() const { return std::tuple_cat(addressWithoutOrdinal(), std::tuple{ordinal}); }
};
struct HeldVoice {
    std::int32_t noteId = -1;
    std::int16_t port = 0, channel = 0, key = 0;
    bool live = false;
    NoteOrigin origin{};
    std::uint32_t sourceOrdinal = 0;
    bool release = false;
    std::int32_t sourceId = -1;  // compiled id, distinct from the running voice id
};
inline bool matchesVoice(const HeldVoice& voice, std::int16_t port, std::int16_t channel,
                         std::int16_t key, std::int32_t noteId) SUPPLE_RT {
    return (port == -1 || voice.port == port) && (channel == -1 || voice.channel == channel) &&
           (key == -1 || voice.key == key) && (noteId == -1 || voice.noteId == noteId);
}

struct Stage {
    host::Instance* instance = nullptr;  // null: placeholder (pass-through)
    Id fx;
    std::vector<ParamLane> lanes;
    bool bypass = false, instrument = false;
    bool autoBypassOnSilence = false, sleeping = false, processed = false;
    std::int64_t inputLatency = 0;  // stage input's timeline shift, also corrects output events
    bool rateLive = false;
    bool callbackBypass = false;  // synchronous bridges never run on the device thread
    double wet = 1.0;
    // The wet level applied at the end of the last block (-1: none yet) and
    // a ramp toward the target (0 when bypassed): a bypass or wet change
    // crossfades over Plan::smoothFrames, the plugin running meanwhile.
    float appliedWet = -1.0f, rampFromWet = 0.0f, rampToWet = 0.0f;
    std::uint32_t wetRampLeft = 0;
    bool preserveDryOnLocate = false;
    std::int64_t latency = 0;
    Delay dry;  // bypassed or partly-dry signal, delayed to match the stage
    Stereo out, dryBuf;
    // The key (ADR-0018): key edges sum here before the stage runs.
    bool keyed = false;
    std::uint32_t keyChannels = 0;  // the plug-in's (1: the key is mixed to mono)
    Stereo key;
    // Pins (ADR-0020): the track channels (0-based) the plug-in reads, writes
    // (-1: none) and keys from. Pinned stages, and every stage of a strip with
    // auxiliary channels, delay the channels they do not write by their
    // latency (chanDelay, one per track channel).
    int inPin[2] = {0, 1}, outPin[2] = {0, 1};
    std::vector<int> keyPins;
    bool pinned = false;
    std::vector<MonoDelay> chanDelay;
    Stereo pinBuf;
    // Auxiliary outputs (ADR-0021): the track channel (0-based; -1: none)
    // each of the plug-in's auxiliary output channels writes, as far as it
    // has them; their buffers for one block and the pointers process() gets.
    std::vector<int> auxOutPin;
    std::vector<std::vector<float>> auxOut;
    std::vector<float*> auxOutPtr;
    bool auxWritten = false;  // the plug-in ran this block: its aux outputs are fresh
    std::vector<host::Event> blockEvents;
    std::vector<HeldVoice> voices, pendingVoices;          // bounded input preflight
    std::vector<NoteSource> noteSources, noteSourceIndex;  // immutable compiled identities
    std::vector<std::int32_t> sourceIds;                   // parallel to this block's inputs
    std::uint32_t nextNoteId = 0;
    std::size_t releases = 0;  // leading addressed releases already use running ids
    std::uint64_t voiceDropped = 0;
    std::vector<host::OutputEvent> outStorage;
    host::OutputEvents outEvents;
    // PDC policy (fx.pdc): an ignored stage keeps its own dry delay but adds
    // nothing to its chain's compensation.
    bool pdcIgnore = false;
    // Parallel groups (fx.parallel): a stage marked parallel runs beside the
    // one before it on the same input; the branches' outputs, each delayed to
    // the group's latency (align), are summed. `group` is the index of the
    // group's first stage (-1: serial), whose groupIn/groupOut hold the block.
    bool parallel = false;
    int group = -1;
    bool groupEnd = false;
    std::int64_t before = 0;  // compensated latency of its chain ahead of its input
    Delay align;
    Stereo groupIn, groupOut;
    // Oversampling (fx.oversampling): the plug-in runs at `os` times the plan
    // rate behind half-band resamplers (L/R in and out, then key L/R up); its
    // latency is padded at the high rate to a whole number of plan samples.
    int os = 1;
    std::vector<dsp::Oversampler> osFilters;
    std::vector<float> osIn[2], osOut[2], osKey[2];
    Delay osPad;
};

// The latency a stage adds to its chain's compensation.
inline std::int64_t compLatency(const Stage& st) { return st.pdcIgnore ? 0 : st.latency; }

struct RateClipState {
    TapeReader tape;
    std::unique_ptr<StretchReader> stretch;
    std::unique_ptr<RateClipState> previous;
    std::vector<std::array<float, kRateQuantum>> output;
    std::vector<float*> pointers;
    std::int64_t quantum = std::numeric_limits<std::int64_t>::min();
    bool primed = false, failed = false;
    bool preserve = false;
    RateClipState(std::shared_ptr<const TapeTable> table, std::size_t channels, double rate,
                  bool mode)
        : tape(std::move(table), channels),
          stretch(mode ? std::make_unique<StretchReader>(rate, channels) : nullptr),
          output(channels),
          pointers(channels),
          preserve(mode) {
        for (std::size_t c = 0; c < channels; ++c) pointers[c] = output[c].data();
    }
};

// An audio item ready to play: which source frames land on which samples.
struct Clip {
    Id item, take;
    std::unique_ptr<RateClipState> rateState;
    std::shared_ptr<const audio::AudioData> data;
    std::int64_t start = 0, length = 0;  // samples on the track's timeline (PDC-shifted)
    std::int64_t deviceShift = 0;
    std::int64_t offset = 0;  // source frame at the item start
    bool loop = false;
    model::ChannelMode mode = model::ChannelMode::Normal;
    double gainL = 1.0, gainR = 1.0;
    std::int64_t fadeIn = 0, fadeOut = 0;
    model::FadeShape fadeInShape = model::FadeShape::Linear,
                     fadeOutShape = model::FadeShape::Linear;
    double fadeInCurve = 0, fadeOutCurve = 0;
    int takeChain = -1;  // >= 0: it plays into that take chain, not the track's sum
};

struct TimedEvent {
    std::int64_t sample;
    host::Event event;
    NoteOrigin origin{};
};

// A take with its own FX chain or envelopes (gap log #22): the item plays
// through it before it reaches the track's input. Its input runs `latency`
// samples ahead (the chain's PDC), its output is cut to the item's span and
// gets the item and take gain, pan, fades and the take's envelopes there.
struct TakeChain {
    Id item, take;
    std::int64_t start = 0, end = 0;  // the item's span, track timeline (PDC-shifted)
    std::int64_t latency = 0;         // the chain's
    int clip = -1;                    // its audio (an index into Node::clips), or -1
    std::vector<TimedEvent> events;   // a MIDI take's, for its instrument, input side
    std::size_t nextEvent = 0;
    bool rateEventFresh = true;
    int noteTarget = -1;
    std::array<std::uint8_t, 16 * 128> held{};
    bool releaseHeld = false;
    std::vector<Stage> chain;
    double gainL = 1.0, gainR = 1.0;  // item gain x take gain, take pan
    std::int64_t fadeIn = 0, fadeOut = 0;
    model::FadeShape fadeInShape = model::FadeShape::Linear,
                     fadeOutShape = model::FadeShape::Linear;
    double fadeInCurve = 0, fadeOutCurve = 0;
    Automation volume, pan, mute;  // the take's envelopes (dB, %, 0/1), output side
    Stereo buf;
    bool active = false;  // processed in the last block: the chain is fresh at each entry
};

struct Node {
    Id id;
    std::optional<model::TrackFreeze> freeze;
    std::shared_ptr<const audio::AudioData> frozenAudio;
    // Auxiliary channels 3.. (ADR-0020): summed from routes, read and written
    // by plug-in pins, read by routes after the chain; never faded, panned or
    // sent to the parent.
    int auxCount = 0;
    std::vector<std::vector<float>> aux;
    float* channel(int c) SUPPLE_RT {
        return c == 0   ? sum.l.data()
               : c == 1 ? sum.r.data()
                        : aux[static_cast<std::size_t>(c - 2)].data();
    }
    std::vector<std::size_t> in;   // incoming edges, in summation order
    std::vector<std::size_t> out;  // source-owned send encoders
    std::vector<Stage> chain;
    // Input FX (a channel's input_chain): they process its record input, which
    // is then monitored and recorded, whenever the track is armed with an
    // audio input. Live (the device input exists only in the callback) and
    // outside compensation; `inputBuf` holds this block's processed input.
    std::vector<Stage> inputChain;
    std::vector<int> recordInputs;  // device channels, 0-based (one = mono to both)
    std::int64_t inputLatency = 0;
    Stereo inputBuf;
    std::int64_t latIn = 0, latChain = 0;  // forward arrival (diagnostic), compensated chain
    // Timeline at this node input is processing position - timelineShift.
    // For paths to main: downstream latency = Plan::outputLatency - timelineShift.
    std::int64_t timelineShift = 0;
    Stereo sum, preFx, preFader, post, postKey;
    std::vector<TimedEvent> events;
    std::size_t nextEvent = 0;
    bool rateEventFresh = true;
    // Notes delivered and not yet ended, per MIDI channel and key; a loop
    // wrap ends them (note-offs at the start of the next block).
    std::array<std::uint8_t, 16 * 128> held{};
    bool releaseHeld = false;
    std::vector<Clip> clips;
    std::vector<TakeChain> takeChains;
    // Masters (ADR-0057): their own items play after the plain gain/pan stage,
    // into `items`, and join `post` there (ramped by the master's mute only);
    // `mix` keeps the output before them, what a master's recording takes.
    // `itemShift`: the timeline shift where the items play (the input's on
    // channels, the input's plus the chain's on masters).
    bool postItems = false;
    std::int64_t itemShift = 0;
    Stereo items, mix;
    // While it records (ADR-0057), a master's items are silent from
    // `silentFrom` to `silentTo` (plan samples of the timeline: the punch
    // range); silent everywhere, they are left out of the plan instead.
    std::int64_t silentFrom = 0, silentTo = 0;
    bool dropItems = false;  // lane_mute, or recording with its items silent everywhere
    bool hasItems() const SUPPLE_RT { return !clips.empty() || !takeChains.empty(); }
    int noteTarget = -1;  // chain index receiving note events
    double volumeDb = 0.0, pan = 0.0;
    double staticGain = 1, staticPan = .5;
    model::StripType type = model::StripType::Channel;
    bool mute = false, master = false, decodeInput = false;
    bool phaseInvert = false, mono = false;
    UtilityControl utility;
    bool soloSilenced = false;
    bool postKeyed = false;
    ConsoleStage<console9::Buss> decoder;
    ConsoleStage<console9::Channel> encoder;
    FaderControl fader;
    ConsoleStage<console9::Channel> keyEncoder;
    FaderControl keyFader;  // mute/fader/pan without audible-send solo suppression
    // Input monitoring (ADR-0014): the device inputs this track hears (0-based;
    // one = mono to both sides), and whether it is live (monitors, or is fed
    // by a node that does).
    std::vector<int> monitorInputs;
    bool tapeAuto = false;  // monitor auto: the input is heard only while stopped or recording
    // MIDI monitoring (ADR-0016): the device and channel mapping this track
    // plays live; the engine resolves the device to ports.
    struct MidiIn {
        std::string device;
        int fromChannel = 0, toChannel = 0;
        std::uint64_t ports = 0;
    };
    std::optional<MidiIn> midiIn;
    std::array<std::uint8_t, 16 * 128> liveHeld{};  // device notes sounding (per channel/key)
    bool live = false;
    Automation autoVolume, autoPan, autoMute;
    bool holdVolume = false, holdPan = false;  // a write pass holds them (ADR-0061)
    bool volumeCurve() const SUPPLE_RT { return autoVolume.active() && !holdVolume; }
    bool panCurve() const SUPPLE_RT { return autoPan.active() && !holdPan; }
    VcaControls vcas;
    std::vector<float> auxPostGain;  // source-owned main-to-aux post-fader gain
    // Metering (engine.md §2.3, P5.3c): the bank slot this strip's post signal
    // is measured into (0xFFFF: unmetered), and the last block's measurements,
    // written by processNode on whichever executor processed it.
    std::uint16_t meterSlot = 0xffff;
    std::uint64_t meterGeneration = 0;
    float meterPeak[2] = {0.0f, 0.0f};
    float meterSumSq[2] = {0.0f, 0.0f};
};

// Every stage of a node: its chain's, then its take chains'.
template <class N, class F>
void forEachStage(N& node, F&& f) {
    for (auto& st : node.inputChain) f(st);
    for (auto& st : node.chain) f(st);
    for (auto& tc : node.takeChains)
        for (auto& st : tc.chain) f(st);
}

// A master whose output is recorded (ADR-0057): armed, or still recording.
// Where the callback runs the node it records the node's buffer; where the
// renderer does, the renderer copies it into the plan's record ring `ring`
// each block and the callback pulls it into `pulled` as it plays.
struct RecordTap {
    std::size_t node = 0;
    int ring = -1;  // index into Plan::recordRings; -1: the callback runs the node
    Stereo pulled;
};

// A master's main pair at the device (ADR-0056): 0-based device channels;
// right < 0 plays mono, (L+R)/2, on `left`.
struct OutputTap {
    std::size_t node = 0;
    int left = 0, right = 1;
};

// Mix values a mix-only edit publishes into a running plan (ADR-0060), and
// the write holds of an automation pass (ADR-0061). The main thread is the
// only writer: it brackets each publication with an odd `sequence` (a
// seqlock). Each executor copies what its nodes and edges read at the start
// of a block (syncMix) when the sequence moved; a copy that raced a
// publication keeps its old sequence and repeats at the next block.
struct MixSlots {
    struct Value {
        std::atomic<double> volumeDb{0}, pan{0};  // a route's pan resolved against its source
        std::atomic<bool> holdVolume{false}, holdPan{false};
    };
    std::unique_ptr<Value[]> strips;  // per node
    std::unique_ptr<Value[]> routes;  // per edge (parent sends keep their compiled values)
    std::unique_ptr<Value[]> vcas;    // per VCA group (pan unused)
    std::vector<Id> vcaIds;           // main thread: the group of each VCA slot
    std::atomic<std::uint64_t> sequence{0};
    std::array<std::uint64_t, 2> seen{};  // per executor class (0: renderer or all, 1: live)
};

struct Plan {
    InstancePool* pool = nullptr;  // main-thread prepare/notifications only
    std::uint64_t generation = 0;  // set by the real-time engine when it publishes the plan
    std::vector<Node> nodes;       // topological order
    std::size_t mainMaster = 0;    // tr_0 need not be last (multiple roots and key routes)
    // The masters that play to the device, in project.masters order. Each one
    // is live whenever anything is, and all of them share outputLatency.
    std::vector<OutputTap> outputs;
    std::vector<Edge> edges;
    bool linearConsole = false;
    model::Playrate masterPlayrate;
    Automation rateEnvelope;
    bool rateEnabled = false;
    // Curves/anchors are complete before publication. Executors advance their
    // disjoint windows and source DSP state; scalar transactions use RT queues.
    std::array<std::unique_ptr<PlayrateTimeline>, 2> timelines, previousTimelines;
    std::array<PlayrateTimeline::Snapshot, 2> rateControl;
    // Main writes before publication; main never reads the renderer/callback
    // handoff fields below. Keep scheduling/transport queries independent of
    // executor adoption, including a seek not yet heard by the callback.
    struct PreparedRateBoundary {
        std::int64_t stream = 0;
        std::uint64_t epoch = 0;
    } preparedRateBoundary;
    std::int64_t rateHandDevice = 0;
    std::uint64_t rateSeekSequence = 0, rateEpochSeekSequence = 0, rateFromGeneration = 0;
    // Prepared from a unity-bypass plan (or none): any unity predecessor plays
    // the same identity trajectory, so the renderer may adopt from either.
    bool rateFromUnity = false;
    bool ratePrepared = false;
    std::int64_t rateOrigin = 0, rateDeviceMargin = 0, rateBackendLatency = 0;
    std::int64_t rateSourceAhead = 0, rateSourceBehind = 0;
    std::int64_t rateSwitchDevice = INT64_MIN, rateSafeDevice = INT64_MIN;
    bool rateControlsChanged = false;
    std::unique_ptr<std::atomic<std::uint64_t>> scalarPublication =
        std::make_unique<std::atomic<std::uint64_t>>(0);
    // Control-thread history retains edits ahead of the consumed snapshot so
    // structural preparation can reconstruct the same future trajectory.
    std::vector<ScalarRateEdit> scalarHistory;
    double requestedScalar = 1;
    double rateSample(std::int64_t device, bool live = false) const SUPPLE_RT {
        return rateEnabled ? timelines[live ? 1 : 0]->position(device) : double(device);
    }
    double rateSeconds(std::int64_t device, bool live = false) const SUPPLE_RT {
        return rateEnabled ? timelines[live ? 1 : 0]->absolutePosition(device) / rate
                           : startSeconds + double(device) / rate;
    }
    double ratePhase(std::int64_t device, bool live = false) const SUPPLE_RT {
        return rateEnabled ? timelines[live ? 1 : 0]->phase(device) : double(device);
    }
    double rate = 48000.0;
    std::uint32_t smoothFrames = 480;  // 10 ms: ramps at plan swaps (carryMix)
    std::unique_ptr<MixSlots> mix = std::make_unique<MixSlots>();
    double startSeconds = 0.0;
    std::int64_t outputLatency = 0;
    std::uint32_t block = 512;
    // Shared with the session version it was compiled from: the plan must not
    // depend on that session staying alive (the real-time engine outlives it).
    std::shared_ptr<const time::TimeMap> timeMap;
    std::vector<std::string> warnings;
    std::filesystem::path projectDir;
    std::shared_ptr<const PreparedSources> preparedSources;
    MediaCache* media = nullptr;
    std::vector<host::Instance*> instances;  // every instance the plan processes, sorted
    std::unique_ptr<TaskGraph> graph;        // node dependencies, for processBlockOn
    bool anyLive = false;                    // some node monitors input
    bool heldLive = false;                   // some node is live only through keepLive
    // The two executors' shares (ADR-0014): the renderer's nodes (all when
    // nothing is live) with their task graph, the callback's live nodes (in
    // order), each side's instances (sorted), and the frontier edges.
    std::vector<std::size_t> rendererNodes, liveNodes;
    std::unique_ptr<TaskGraph> rendererGraph, liveGraph;
    std::vector<host::Instance*> rendererInstances, liveInstances;
    // Metering (P5.3c): the metered nodes each executor measures, filled with
    // the slots by the engine's load() on the main thread, so an executor
    // iterates a list after a block instead of scanning every node.
    std::vector<std::size_t> rendererMeterNodes, liveMeterNodes, meterNodes;
    std::vector<std::size_t> frontier;
    std::unique_ptr<ChunkRing> rateOutput;  // canonical rate blocks, nonsplit renderer only
    std::vector<std::unique_ptr<ChunkRing>> frontierRings;  // allocated by the engine
    // Recorded masters (ADR-0057), in node order; their rings are allocated
    // by the engine for the taps the renderer runs, like frontier rings.
    std::vector<RecordTap> recordTaps;
    std::vector<std::unique_ptr<ChunkRing>> recordRings;
    // Split: the renderer hands the callback something (frontier or record rings).
    bool feedsCallback() const SUPPLE_RT { return !frontier.empty() || !recordRings.empty(); }
    // Rate plans: prepared by the control thread before publication. Unity
    // plans: filled by the renderer before its callback handoff (ADR-0014/15).
    // Stream position is in frames into the transport epoch.
    std::int64_t handAt = 0;
    std::uint64_t handEpoch = 0;
    // Two executors at work (anticipative engine with live nodes): frontier
    // edges read their rings, not their sources' taps.
    bool split = false;
    // The device's input for this cycle, set by the audio callback before it
    // processes live nodes (null: no device, e.g. offline).
    const float* const* deviceIn = nullptr;
    std::uint32_t deviceInChannels = 0;
    bool deviceRecording = false;  // a take is being recorded (auto monitoring hears the input)
    // The callback's device MIDI (frames within the cycle) and where the part
    // of the cycle being processed starts.
    std::span<const MidiInputEvent> deviceMidi;
    std::uint32_t deviceMidiOffset = 0;
    // Armed channels with input FX: what the recorder takes instead of the
    // raw device input (filled by prepare(), pointing into the nodes).
    std::vector<ProcessedInput> processedInputs;
};

// Source-only preparation; caller runs this off the callback before making a
// seek epoch or a changed plan audible. Renderer/live state is disjoint.
Status prepareRateSources(Plan& p, std::int64_t device, double timeline,
                          const LoopRange* loop = nullptr);
Status primeRateSources(Plan& p, std::int64_t device);
void prepareRateWindow(Plan& p, std::int64_t pos, std::uint32_t frames, int liveClass,
                       const LoopRange* loop = nullptr) SUPPLE_RT;

void playRateClip(Plan& p, Clip& clip, Stereo& into, std::int64_t pos, std::uint32_t frames,
                  std::int64_t ahead, bool live) SUPPLE_RT;

std::uint32_t rateUntil(Plan& p, std::int64_t pos, std::uint32_t frames, const LoopRange& loop,
                        int liveClass) SUPPLE_RT;
bool rateWrapped(const Plan& p, std::int64_t from, std::int64_t to, const LoopRange& loop,
                 bool live) SUPPLE_RT;
enum class RateAdoption { Wait, Ready, Reseek };
RateAdoption rateAdoption(const Plan& p, const Plan* previous, std::int64_t stream,
                          std::uint64_t epoch, bool playing) SUPPLE_RT;

// What a recorded master records (ADR-0057): its output before its own items.
inline const Stereo& recordSignal(const Node& n) SUPPLE_RT {
    return n.postItems && n.hasItems() ? n.mix : n.post;
}

// Structural invariant checker, also exposed to the randomized plan tests.
Status checkStrips(const Plan& p);
const Stereo& edgeSignal(const Plan& p, const Edge& e) SUPPLE_RT;

// Builds the plan's structure (nodes in topological order, edges, plugin
// instances from the pool, automation). `startSeconds` is the timeline
// origin of sample 0 (0 for the real-time engine). `rate`: the rate the plan
// runs at (0: the project's; the real-time engine runs at the device's —
// positions are in seconds and media is resampled, so any rate works).
// `keepLive`: tracks that stay in the live region even unmonitored (sorted).
// `recording`: masters recording their output (ADR-0057): each gets a record
// tap (as every armed master does) and its items are silent where it records.
Result<Plan> compile(const Session& s, InstancePool& pool, double startSeconds, std::uint32_t block,
                     double rate = 0, const std::vector<model::Id>* keepLive = nullptr,
                     std::span<const RecordingMaster> recording = {});
// Main thread: activates new instances, measures latencies, sizes buffers
// and delays, compiles events and clips.
Status prepare(Plan& p, const Session& s);
// Audio thread: one block of node `index` at plan sample `pos` (its sources
// must have been processed for this block). The node, its plug-ins included,
// runs with FTZ/DAZ set; the caller's MXCSR is restored (ADR-0025).
// `self`: with several executors, only stages whose instance this executor
// owns are processed (the rest pass through); null: no ownership check.
bool processNode(Plan& p, std::size_t index, std::int64_t pos, std::uint32_t frames,
                 const host::Transport& transport, const void* self = nullptr) SUPPLE_RT;
// Audio thread: one block of every node at plan sample `pos`, in order.
// `playing` false: the transport is stopped — plugins and monitored inputs
// run, items and MIDI do not play (and `pos` should not advance).
bool processBlock(Plan& p, std::int64_t pos, std::uint32_t frames, bool playing = true,
                  const LoopRange* loop = nullptr) SUPPLE_RT;
// The same on the worker pool (the caller helps): bit-identical results.
// Not SUPPLE_RT itself (it wakes the helpers); every node task is.
bool processBlockOn(Workers& workers, Plan& p, std::int64_t pos, std::uint32_t frames);
// Callback fork-join; split selects the live subgraph, otherwise all nodes.
// Transport/device input and buffers stay immutable until all helpers depart.
bool processCallbackBlock(LiveWorkers* workers, Plan& p, std::int64_t pos, std::uint32_t frames,
                          bool playing, bool split, const void* self,
                          const LoopRange* loop = nullptr) SUPPLE_RT;
// The renderer's nodes only (rendererGraph), serially without helpers.
bool processRendererBlock(Workers* workers, Plan& p, std::int64_t pos, std::uint32_t frames,
                          const void* self, const LoopRange* loop = nullptr, bool playing = true);
// Repositions the cursors and clears the delay lines of the live nodes (or
// of the others) and of the edges into them.
void locateClass(Plan& p, std::int64_t pos, bool live, bool restartConsole = true) SUPPLE_RT;
// Audio thread: repositions every playback cursor at `pos` (events,
// automation) and clears delay lines. Plugins are not touched. Plan adoption
// passes restartConsole=false to preserve console state and mixer ramps.
void locate(Plan& p, std::int64_t pos, bool restartConsole = true) SUPPLE_RT;
// A loop wrap (ADR-0015): moves the event and automation cursors of one
// class of nodes (0: the renderer's, 1: live, -1: all) to `pos`, keeping
// delay lines and plugin tails, restarting console seeds; notes held at the wrap get
// note-offs at the start of the next block.
void wrapTo(Plan& p, std::int64_t pos, int liveClass) SUPPLE_RT;
// A plan swap (ADR-0016): device notes still sounding on a track carry over
// to the new plan's node, which releases them if it no longer plays MIDI
// live (so monitoring off never leaves a note hanging). `liveOnly`: only
// into live nodes (the anticipative callback; a node that left it belongs to
// the renderer, and its instance was reset on the way).
void carryLiveNotes(Plan& from, Plan& to, bool liveOnly) SUPPLE_RT;
// A plan swap (the 10 ms crossfade on structural swaps, engine.md §2): the
// Console 9 state, fader ramps, stage wet levels and matching dry/channel
// delay histories carry into the new
// one's matching nodes (by track) and stages (by instance), so a changed
// value ramps from where the old plan left it. `liveClass` as for wrapTo.
Status configureRateTransition(Plan& p, Plan* previous, PlayrateTimeline& old, std::int64_t device);
void carryRateSources(Plan& from, Plan& to, int liveClass) SUPPLE_RT;
void carryMix(Plan& from, Plan& to, int liveClass) SUPPLE_RT;

// A strip's static fader gain (masters: plain gain; channels and busses: the
// Console 9 fader law) and a send's (route plus, post fader, source level).
double stripGain(bool master, double db) SUPPLE_RT;
double sendGain(double routeDb, model::Tap tap, double sourceDb) SUPPLE_RT;
// Mix-value publication (ADR-0060). `holds`: the volume/pan targets a write
// pass holds (ADR-0061). Main thread, before or after the plan is published.
void publishMix(Plan& p, const model::Session& s, std::span<const model::EnvTarget> holds);
// An executor's copy of the published values into the nodes and edges its
// class reads (`liveClass` as for wrapTo), at the start of a block.
void syncMix(Plan& p, int liveClass) SUPPLE_RT;
// The device output for `k` frames (ADR-0056): every output master's main
// pair at its channels (taps beyond `width` are skipped), the rest silent.
// `out[c] + offset` receives channel c. A channel's first tap is copied, so a
// single master reaches the device bit-exact; further taps add.
void writeOutputs(const Plan& p, float* const* out, std::uint32_t width, std::uint32_t offset,
                  std::uint32_t k) SUPPLE_RT;

// `loop`: the executor's transport loop, for plugins that follow it.
host::Transport transportAt(const Plan& p, std::int64_t sample, const LoopRange* loop = nullptr,
                            bool live = false) SUPPLE_RT;

}  // namespace supple::engine::detail
