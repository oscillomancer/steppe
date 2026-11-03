// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// item.export: an item's raw material to a file of the caller's choosing (the
// item menu's Export…). Audio: the active take's source samples over the span
// the item reads, unprocessed, as WAV. MIDI: what the item plays, as a
// Standard MIDI File. Reads the session; changes nothing in it.
#include <algorithm>
#include <cctype>
#include <cmath>

#include "heard_midi.h"
#include "supple/audio/audio_file.h"
#include "supple/base/files.h"
#include "supple/engine/app.h"
#include "supple/engine/offline.h"
#include "supple/midi/midi.h"
#include "supple/model/audio_warp.h"

namespace supple::engine {
namespace {
namespace fs = std::filesystem;
using model::Id;

struct ExportArgs {
    std::string item, path;
};
struct ExportResult {
    std::string path, format;
    std::optional<std::string> sampleFormat;
    std::optional<double> sampleRate;
    std::optional<std::int64_t> channels, frames, notes;
};

// The in-memory sample budget the renderer and source preparation use.
constexpr std::int64_t kMaxSamples = std::int64_t(1) << 28;

fs::path mediaPath(const EngineApp& app, const model::MediaSource& md) {
    fs::path path = md.path;
    if (path.is_relative()) {
        const auto inMedia = app.mediaDir() / path;
        std::error_code ec;
        path = fs::exists(inMedia, ec) ? inMedia : app.projectDir() / path;
    }
    return path;
}

std::string lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// The file to write: relative paths resolve against the work directory, no
// extension gets the kind's own, another one is refused.
Result<fs::path> target(const EngineApp& app, const model::Session& s, const std::string& text,
                        bool midi) {
    if (text.empty())
        return Error("item.export_path", "no file to write", "/path",
                     std::string("give a file path ending in ") + (midi ? ".mid" : ".wav"));
    fs::path path = app.resolve(fs::path(text));
    const std::string ext = lower(path.extension().string());
    if (ext.empty())
        path += midi ? ".mid" : ".wav";
    else if (midi ? ext != ".mid" && ext != ".midi" : ext != ".wav")
        return Error("item.export_extension",
                     std::string(midi ? "a MIDI item exports a MIDI file, not "
                                      : "an audio item "
                                        "exports a WAV "
                                        "file, not ") +
                         ext,
                     "/path", midi ? "end the path in .mid" : "end the path in .wav");
    // Never over the project's own media (the item's source among them).
    std::error_code ec;
    if (fs::exists(path, ec))
        for (const auto& e : s.media)
            if (fs::equivalent(path, mediaPath(app, *e.value), ec))
                return Error("item.export_path", "the file is the project's media " + e.id.str(),
                             "/path", "choose another file name");
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    return path;
}

Result<ExportResult> exportAudio(const EngineApp& app, const model::Session& s,
                                 const model::Item& it, const model::Take& take,
                                 const std::string& file) {
    const model::MediaSource* md = s.media.find(*take.source);
    if (!md)
        return Error("item.export_source", "the take's media is missing from the project", "/item",
                     "relink the item's source or choose another take");
    auto path = target(app, s, file, false);
    if (!path) return path.error();
    const fs::path source = mediaPath(app, *md);
    auto info = audio::inspect(source);
    auto data = audio::readAudio(source);
    if (!info || !data) {
        Error e = info ? data.error() : info.error();
        return Error("item.export_source", e.message, "/item",
                     "restore or relink the source file " + source.string());
    }
    // The source span the item reads, in source seconds: a warp map's
    // positions at the item's ends (a looping item plays unwarped), else the
    // take's offset and the source time its rate (and an unpreserved pitch)
    // passes over the item's length. Raw source samples are written: the rate,
    // pitch and warp themselves are not applied.
    const model::ItemSpan span = model::itemSpan(s, it);
    const double length = span.endSec - span.startSec;
    double from = take.startOffset, to = 0;
    if (model::activeWarp(take) && !it.loop) {
        from = model::audioSourceAt(take, 0);
        to = model::audioSourceAt(take, length);
    } else {
        const MediaVariant variant{take.playrate, take.pitch, take.preservePitch, take.pitchMode};
        to = from + length * variant.speed();
    }
    const double rate = data->sampleRate;
    const std::int64_t first = time::samplesAt(from, rate);
    const std::int64_t frames = time::samplesAt(to, rate) - first;
    const auto channels = static_cast<std::int64_t>(data->channelCount());
    const auto available = static_cast<std::int64_t>(data->frames());
    if (frames <= 0 || channels == 0)
        return Error("item.export_empty", "the item covers no source audio", "/item",
                     "lengthen the item or move its source offset onto the audio");
    if (frames > kMaxSamples / channels)
        return Error("item.export_too_long", "the export exceeds the in-memory sample budget",
                     "/item", "shorten the item (a long loop repeats its source in the file)");
    // Outside the source: silence, as the item plays it; a looping item
    // wraps around the source instead.
    audio::AudioData out;
    out.sampleRate = rate;
    out.channels.assign(static_cast<std::size_t>(channels),
                        std::vector<float>(static_cast<std::size_t>(frames), 0.f));
    for (std::size_t c = 0; c < out.channels.size(); ++c) {
        const auto& in = data->channels[c];
        auto& o = out.channels[c];
        for (std::int64_t i = 0; i < frames; ++i) {
            std::int64_t f = first + i;
            if (it.loop && available > 0) f = ((f % available) + available) % available;
            if (f >= 0 && f < available)
                o[static_cast<std::size_t>(i)] = in[static_cast<std::size_t>(f)];
        }
    }
    // A 16- or 24-bit PCM WAV stays so (exact: no dither); anything else is float.
    audio::WavOptions options;
    options.dither = false;
    options.format = audio::SampleFormat::Float32;
    std::string format = "float32";
    if (info->container == audio::Container::Wav && !info->floatSamples &&
        (info->bitsPerSample == 16 || info->bitsPerSample == 24)) {
        options.format =
            info->bitsPerSample == 16 ? audio::SampleFormat::Pcm16 : audio::SampleFormat::Pcm24;
        format = info->bitsPerSample == 16 ? "pcm16" : "pcm24";
    }
    if (auto written = audio::writeWav(*path, out, options); !written) {
        Error e = written.error();
        e.path = "/path";
        if (e.hint.empty()) e.hint = "choose a writable folder with free space";
        return e;
    }
    ExportResult result;
    result.path = path->string();
    result.format = "wav";
    result.sampleFormat = format;
    result.sampleRate = rate;
    result.channels = channels;
    result.frames = frames;
    return result;
}

Result<ExportResult> exportMidi(const EngineApp& app, const model::Session& s,
                                const model::Item& it, const model::Take& take,
                                const std::string& file) {
    const model::MidiSource* src = s.midi.find(*take.source);
    if (!src)
        return Error("item.export_source", "the take's MIDI is missing from the project", "/item",
                     "choose another take");
    auto path = target(app, s, file, true);
    if (!path) return path.error();
    const midi::Sequence& seq = src->sequence;
    midi::Sequence out;
    out.ppq = seq.ppq > 0 ? seq.ppq : 960;
    detail::heardMidi(s, it, take, seq, 0, out);
    const model::ItemSpan span = model::itemSpan(s, it);
    const double beats = s.timeMap->beatAt(span.endSec) - span.startBeat;
    out.length = std::max<std::int64_t>(0, std::llround(beats * out.ppq));
    out.canonicalize();
    const time::TimeMap& tm = *s.timeMap;
    const auto meter = tm.meterAt(tm.barBeat(span.startBeat).bar);
    auto bytes = midi::writeSmf(out, {{0, midi::Meter{meter.numerator, meter.denominator}}},
                                {{0, tm.bpmAt(span.startBeat)}});
    if (!bytes) {
        Error e = bytes.error();
        e.path = "/item";
        return e;
    }
    if (auto written = writeFileAtomic(*path, *bytes); !written) {
        Error e = written.error();
        e.path = "/path";
        if (e.hint.empty()) e.hint = "choose a writable folder";
        return e;
    }
    ExportResult result;
    result.path = path->string();
    result.format = "mid";
    result.notes = static_cast<std::int64_t>(out.notes.size());
    return result;
}
}  // namespace

void registerExportCommands(cmd::Registry& registry) {
    cmd::Command<ExportArgs, ExportResult> c;
    c.name = "item.export";
    c.kind = cmd::Kind::Action;
    c.exposure = cmd::Exposure::McpTool;
    c.summary = "Write an item's raw audio (WAV) or its MIDI (SMF) to a file.";
    c.description =
        "Exports the active take over the part the item covers (trims and splits respected). "
        "Audio: the source's own samples, rate and channels, from the source position at the "
        "item's start to the one at its end (playrate, unpreserved pitch and stretch markers "
        "decide that span but are not applied; a looping item ignores markers and repeats its "
        "source as heard; outside the source is silence). No gain, fades, pan, channel mode, FX "
        "or track processing. 16/24-bit PCM WAV sources keep their format; others are 32-bit "
        "float. MIDI: what the item plays (loops unrolled, notes ending by its end) from 0, at "
        "the source PPQ, with the tempo and meter at the item's start. Relative paths resolve "
        "against the work directory; a missing extension is added; existing files are replaced, "
        "the project's media never.";
    c.tags = {"item", "export"};
    c.args =
        schema::object<ExportArgs>("item_export_args")
            .field("item", &ExportArgs::item, schema::string().pattern("^it_[0-9]+$", "item id"))
            .field("path", &ExportArgs::path, schema::string())
            .build();
    c.result = schema::object<ExportResult>("item_export_result")
                   .field("path", &ExportResult::path, schema::string())
                   .field("format", &ExportResult::format, schema::string())
                   .field("sample_format", &ExportResult::sampleFormat, schema::string())
                   .field("sample_rate", &ExportResult::sampleRate, schema::number())
                   .field("channels", &ExportResult::channels, schema::integer())
                   .field("frames", &ExportResult::frames, schema::integer())
                   .field("notes", &ExportResult::notes, schema::integer())
                   .build();
    c.errors = {
        {"item.unknown", "no such item", "use an id from session.query {kind: \"item\"}"},
        {"item.export_empty", "an empty take, or no source audio under the item",
         "pick a take with material (take.activate) or move the item onto its audio"},
        {"item.export_source", "the source file cannot be read", "restore or relink the source"},
        {"item.export_path", "no path, or it names the project's media", "choose another file"},
        {"item.export_extension", "the extension does not match the item's kind",
         "use .wav for audio items and .mid for MIDI items"},
        {"item.export_too_long", "the export exceeds the in-memory sample budget",
         "shorten the item"}};
    c.examples = {
        {"The right half of a split item: one second of its source",
         *json::parse(R"({"item":"it_2","path":"right-half.wav"})"),
         *json::parse(
             R"({"path":"<any>","format":"wav","sample_format":"float32","sample_rate":48000.0,"channels":2,"frames":48000})"),
         "song_split_audio"},
        {"A MIDI item as a Standard MIDI File", *json::parse(R"({"item":"it_1","path":"bass"})"),
         *json::parse(R"({"path":"<any>","format":"mid","notes":2})"), "song_with_item"}};
    c.run = [](const ExportArgs& a, cmd::Context& ctx) -> Result<ExportResult> {
        auto& app = ctx.service<EngineApp>();
        const model::Session s = app.view();
        const auto id = Id::parse(a.item);
        const model::Item* it = id && id->kind == model::Kind::Item ? s.items.find(*id) : nullptr;
        if (!it)
            return Error("item.unknown", "no " + a.item, "/item",
                         "use an id from session.query {kind: \"item\"}");
        const model::Take* take = it->activeTake ? s.takes.find(*it->activeTake) : nullptr;
        if (!take || !take->source)
            return Error("item.export_empty", a.item + " has no material to export", "/item",
                         "pick a take with material (take.activate) or record into the item");
        if (take->source->kind == model::Kind::Midi) return exportMidi(app, s, *it, *take, a.path);
        return exportAudio(app, s, *it, *take, a.path);
    };
    registry.add(std::move(c));
}
}  // namespace supple::engine
