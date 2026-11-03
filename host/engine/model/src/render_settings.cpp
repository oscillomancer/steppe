// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/model/render_settings.h"

#include <array>
#include <cmath>

namespace supple::model::render {
namespace {
using schema::Schema;

constexpr std::array<std::string_view, 20> kFields{"source",
                                                   "bounds",
                                                   "start_beat",
                                                   "end_beat",
                                                   "tail_s",
                                                   "pattern",
                                                   "sample_rate",
                                                   "channels",
                                                   "outputs",
                                                   "normalize",
                                                   "limit_true_peak_db",
                                                   "trim",
                                                   "seamless_loop",
                                                   "handles_s",
                                                   "stems",
                                                   "matrix",
                                                   "if_exists",
                                                   "metadata",
                                                   "report",
                                                   "add_to_project"};
constexpr std::array<std::int64_t, 14> kMp3Bitrates{32,  40,  48,  56,  64,  80,  96,
                                                    112, 128, 160, 192, 224, 256, 320};

Schema<std::int64_t> oneOfIntegers(std::string title, std::span<const std::int64_t> allowed,
                                   std::string hint) {
    const auto integer = schema::integer();
    auto spec = integer.jsonSchema();
    spec["enum"] = json::Value::array();
    std::vector<std::int64_t> values(allowed.begin(), allowed.end());
    for (const auto value : values) spec["enum"].asArray().push_back(value);
    return {std::move(title),
            [integer, values, hint](const json::Value& v, std::int64_t& out, schema::DecodeCtx& ctx,
                                    const std::string& path) {
                if (!integer.decodeInto(v, out, ctx, path)) return false;
                if (std::find(values.begin(), values.end(), out) == values.end()) {
                    ctx.error("schema.enum", "unsupported value " + std::to_string(out), path,
                              hint);
                    return false;
                }
                return true;
            },
            [integer](std::int64_t v) { return integer.encode(v); }, spec};
}

// File-name-safe text: no path separators, control characters or characters
// Windows refuses.
Schema<std::string> suffixSchema() {
    return schema::string()
        .pattern("^[A-Za-z0-9 _.+-]{1,32}$", "letters, digits, space, _ . + -")
        .doc("Inserted before the extension of a second or later output, e.g. \"-16bit\"");
}

Schema<int> channelsSchema() {
    json::Value spec = json::Value::object();
    json::Value count = json::Value::object();
    count["type"] = "integer";
    count["enum"] = json::Array{json::Value(1), json::Value(2)};
    json::Value track = json::Value::object();
    track["type"] = "string";
    track["enum"] = json::Array{json::Value("track")};
    spec["anyOf"] = json::Array{std::move(count), std::move(track)};
    spec["description"] =
        "1 (mono, (L+R)/2), 2 (stereo) or \"track\" (stems: mono files for 1-channel tracks)";
    return {"channels",
            [](const json::Value& v, int& out, schema::DecodeCtx& ctx, const std::string& path) {
                if (v.isString() && v.asString() == "track") {
                    out = kChannelsPerTrack;
                    return true;
                }
                if ((v.isInt() || (v.isNumber() && v.asDouble() == std::floor(v.asDouble()))) &&
                    (v.asDouble() == 1 || v.asDouble() == 2)) {
                    out = static_cast<int>(v.asDouble());
                    return true;
                }
                ctx.error("schema.type", "channels must be 1, 2 or \"track\"", path,
                          "use 1 for mono, 2 for stereo, or \"track\" for stems that follow "
                          "their tracks");
                return false;
            },
            [](int v) { return v == kChannelsPerTrack ? json::Value("track") : json::Value(v); },
            spec};
}

Schema<Mp3> mp3Schema() {
    return schema::object<Mp3>("mp3 options")
        .field("mode", &Mp3::mode,
               schema::enumeration<Mp3Mode>({{Mp3Mode::Cbr, "cbr"}, {Mp3Mode::Vbr, "vbr"}})
                   .doc("cbr (default) or vbr"))
        .field("bitrate_kbps", &Mp3::bitrateKbps,
               oneOfIntegers("bitrate", kMp3Bitrates,
                             "use a constant bit rate LAME supports: 32, 40, 48, 56, 64, 80, 96, "
                             "112, 128, 160, 192, 224, 256 or 320")
                   .doc("CBR bit rate [320]"))
        .field("vbr_quality", &Mp3::vbrQuality,
               schema::integer().range(0, 9).doc("VBR quality, 0 (best) to 9 [2]"))
        .build();
}

Schema<Pattern> patternSchemaImpl() {
    auto perSource = schema::object<Pattern>("pattern per source")
                         .field("mix", &Pattern::mix, schema::string())
                         .field("stems", &Pattern::stems, schema::string())
                         .field("regions", &Pattern::regions, schema::string())
                         .build();
    json::Value spec = json::Value::object();
    json::Value text = json::Value::object();
    text["type"] = "string";
    spec["anyOf"] = json::Array{std::move(text), perSource.jsonSchema()};
    spec["description"] =
        "File name pattern (session-model §7.3; may contain / for subfolders), for every "
        "source or as {mix, stems, regions}";
    return {
        "pattern",
        [perSource](const json::Value& v, Pattern& out, schema::DecodeCtx& ctx,
                    const std::string& path) {
            if (v.isString()) {
                out = Pattern{};
                out.all = v.asString();
                return true;
            }
            if (!v.isObject()) {
                ctx.error("schema.type", "pattern must be a string or {mix, stems, regions}", path,
                          "for example \"{project}-v{version}\"");
                return false;
            }
            return perSource.decodeInto(v, out, ctx, path);
        },
        [perSource](const Pattern& p) { return p.all ? json::Value(*p.all) : perSource.encode(p); },
        spec};
}

Schema<Metadata> metadataSchema() {
    const auto text = [] { return schema::string().maxLength(1024); };
    return schema::object<Metadata>("render metadata")
        .doc("Tags written into each file; text members accept the file-name tokens")
        .field("title", &Metadata::title, text())
        .field("artist", &Metadata::artist, text())
        .field("album", &Metadata::album, text())
        .field("track_number", &Metadata::trackNumber, schema::integer().range(1, 9999))
        .field("date", &Metadata::date,
               schema::string().pattern("^[0-9]{4}(-[0-9]{2}-[0-9]{2})?$", "YYYY or YYYY-MM-DD"))
        .field("genre", &Metadata::genre, text())
        .field("comment", &Metadata::comment, text())
        .field("isrc", &Metadata::isrc,
               schema::string().pattern("^[A-Z]{2}[A-Z0-9]{3}[0-9]{7}$",
                                        "a 12-character ISRC like USRC17607839"))
        .field("cover", &Metadata::cover,
               schema::string().maxLength(1024).doc(
                   "A PNG or JPEG of at most 10 MiB, relative to the project folder"))
        .field("bwf", &Metadata::bwf,
               schema::boolean().doc("WAV: a BWF bext chunk (v2) with time reference and "
                                     "loudness"))
        .field("markers", &Metadata::markers,
               schema::boolean().doc("WAV cue points / AIFF markers for the markers and regions "
                                     "in the file's range"))
        .build();
}

Status typeError(const std::string& path, std::string expected) {
    return Error("schema.type", "expected " + expected, path,
                 "use the render settings of session-model §1.10");
}
}  // namespace

std::span<const std::string_view> fieldNames() { return kFields; }
std::span<const std::int64_t> mp3Bitrates() { return kMp3Bitrates; }

std::string_view formatName(Format format) {
    switch (format) {
        case Format::Wav:
            return "wav";
        case Format::Aiff:
            return "aiff";
        case Format::Flac:
            return "flac";
        case Format::Mp3:
            return "mp3";
        case Format::Vorbis:
            return "vorbis";
    }
    return "wav";
}

std::string_view extension(Format format) {
    switch (format) {
        case Format::Wav:
            return ".wav";
        case Format::Aiff:
            return ".aif";
        case Format::Flac:
            return ".flac";
        case Format::Mp3:
            return ".mp3";
        case Format::Vorbis:
            return ".ogg";
    }
    return ".wav";
}

Schema<Format> formatSchema() {
    return schema::enumeration<Format>({{Format::Wav, "wav"},
                                        {Format::Aiff, "aiff"},
                                        {Format::Flac, "flac"},
                                        {Format::Mp3, "mp3"},
                                        {Format::Vorbis, "vorbis"}});
}

Schema<Output> outputSchema() {
    return schema::object<Output>("render output")
        .doc("One encoding of the rendered audio")
        .field("format", &Output::format, formatSchema())
        .field("bit_depth", &Output::bitDepth,
               oneOfIntegers("bit depth", std::array<std::int64_t, 3>{16, 24, 32},
                             "use 16 or 24, or 32 (float) for WAV")
                   .doc("WAV 16/24/32 (float) [32]; AIFF and FLAC 16/24 [24]; none for MP3 and "
                        "Vorbis"))
        .field("dither", &Output::dither,
               schema::enumeration<Dither>(
                   {{Dither::None, "none"}, {Dither::Tpdf, "tpdf"}, {Dither::Shaped, "shaped"}})
                   .doc("16- and 24-bit output: none, tpdf (default) or shaped"))
        .field("sample_rate", &Output::sampleRate,
               schema::integer().range(8000, 384000).doc("This output's own rate"))
        .field("mp3", &Output::mp3, mp3Schema())
        .field("vorbis_quality", &Output::vorbisQuality,
               schema::number().range(-1, 10).doc("Ogg Vorbis quality, -1 to 10 [6]"))
        .field("suffix", &Output::suffix, suffixSchema())
        .build();
}

Schema<Normalize> normalizeSchema() {
    return schema::object<Normalize>("normalize")
        .field("mode", &Normalize::mode,
               schema::enumeration<NormalizeMode>({{NormalizeMode::None, "none"},
                                                   {NormalizeMode::LufsI, "lufs_i"},
                                                   {NormalizeMode::LufsS, "lufs_s"},
                                                   {NormalizeMode::LufsM, "lufs_m"},
                                                   {NormalizeMode::Peak, "peak"},
                                                   {NormalizeMode::TruePeak, "true_peak"},
                                                   {NormalizeMode::Rms, "rms"}}))
        .field("target", &Normalize::target,
               schema::number().range(-100, 0).doc("LUFS or dB [-14]"))
        .field("only_reduce", &Normalize::onlyReduce,
               schema::boolean().doc("Never raise the level"))
        .field("scope", &Normalize::scope,
               schema::enumeration<Scope>({{Scope::File, "file"}, {Scope::Master, "master"}})
                   .doc("file (default), or master: stems share the master mix's gain"))
        .build();
}

Schema<Stems> stemsSchema() {
    return schema::object<Stems>("stems")
        .field("mode", &Stems::mode,
               schema::enumeration<StemMode>(
                   {{StemMode::Isolated, "isolated"}, {StemMode::ThroughMaster, "through_master"}})
                   .doc("isolated (default; no master processing) or through_master"))
        .field("tap", &Stems::tap,
               schema::enumeration<StemTap>({{StemTap::PostFader, "post_fader"},
                                             {StemTap::PreFader, "pre_fader"},
                                             {StemTap::PreFx, "pre_fx"}}))
        .field("tracks", &Stems::tracks,
               schema::arrayOf(schema::string().pattern("^tr_[0-9]+$", "track id"))
                   .maxItems(4096)
                   .doc("Track ids (default: every top-level track)"))
        .field("skip_silent", &Stems::skipSilent,
               schema::boolean().doc("Leave out stems whose peak is below -90 dBFS"))
        .build();
}

Schema<Pattern> patternSchema() { return patternSchemaImpl(); }

Schema<Settings> settingsSchema() {
    static const Schema<Settings> s = [] {
        return schema::object<Settings>("render settings")
            .doc("Render settings (session-model §1.10); every member optional")
            .field(
                "source", &Settings::source,
                schema::enumeration<Source>(
                    {{Source::Mix, "mix"}, {Source::Stems, "stems"}, {Source::Regions, "regions"}})
                    .doc("The dialog's source; render.mix always renders the mix"))
            .field("bounds", &Settings::bounds,
                   schema::enumeration<Bounds>({{Bounds::Project, "project"},
                                                {Bounds::Loop, "loop"},
                                                {Bounds::Custom, "custom"}})
                       .doc("project (default), loop or custom (start_beat, end_beat)"))
            .field("start_beat", &Settings::startBeat, schema::number().range(0, 1e9))
            .field("end_beat", &Settings::endBeat, schema::number().exclusiveMin(0).max(1e9))
            .field("tail_s", &Settings::tailS,
                   schema::number().range(0, 600).doc("Seconds rendered past the end"))
            .field("pattern", &Settings::pattern, patternSchemaImpl())
            .field("sample_rate", &Settings::sampleRate,
                   schema::integer().range(8000, 384000).doc("Default: the project rate"))
            .field("channels", &Settings::channels, channelsSchema())
            .field("outputs", &Settings::outputs,
                   schema::arrayOf(outputSchema())
                       .minItems(1)
                       .maxItems(kMaxOutputs)
                       .doc("1-4 encodings of the same audio [[{format: wav, bit_depth: 32}]]"))
            .field("normalize", &Settings::normalize, normalizeSchema())
            .field("limit_true_peak_db", &Settings::limitTruePeakDb,
                   schema::number().range(-60, 0).doc("A true-peak ceiling (dBTP)"))
            .field("trim", &Settings::trim,
                   schema::object<Trim>("trim")
                       .field("start", &Trim::start, schema::boolean())
                       .field("end", &Trim::end, schema::boolean())
                       .field("threshold_db", &Trim::thresholdDb,
                              schema::number().range(-144, 0).doc("[-60]"))
                       .build())
            .field("seamless_loop", &Settings::seamlessLoop,
                   schema::boolean().doc("Sum the tail into the start (needs tail_s)"))
            .field("handles_s", &Settings::handlesS,
                   schema::number().range(0, 60).doc("Regions: seconds added at both ends"))
            .field("stems", &Settings::stems, stemsSchema())
            .field(
                "matrix", &Settings::matrix,
                schema::mapOf(schema::arrayOf(schema::string().pattern("^tr_[0-9]+$", "track id")))
                    .keyPattern("^project$|^rg_[0-9]+$", "region id or project"))
            .field("if_exists", &Settings::ifExists,
                   schema::enumeration<IfExists>({{IfExists::Overwrite, "overwrite"},
                                                  {IfExists::Number, "number"},
                                                  {IfExists::Skip, "skip"},
                                                  {IfExists::Error, "error"}}))
            .field("metadata", &Settings::metadata, metadataSchema())
            .field("report", &Settings::report,
                   schema::boolean().doc("Write <file>.loudness.txt beside each file"))
            .field("add_to_project", &Settings::addToProject, schema::boolean())
            .build();
    }();
    return s;
}

json::Value toJson(const Settings& settings) { return settingsSchema().encode(settings); }

Status migrateLegacy(json::Value& render, const std::string& path) {
    if (!render.isObject()) return okStatus();
    auto at = [&](std::string_view key) { return json::pointerJoin(path, key); };
    auto& members = render.asObject();
    const json::Value* format = render.find("format");
    const json::Value* depth = render.find("bit_depth");
    const json::Value* secondary = render.find("secondary");
    if (format && !format->isString() && !format->isObject())
        return typeError(at("format"), "a format name or {container, bit_depth, dither}");
    if (depth && !depth->isNumber()) return typeError(at("bit_depth"), "an integer");
    if (secondary && !secondary->isNull() && !secondary->isString() && !secondary->isObject())
        return typeError(at("secondary"), "{format, bit_depth} or \"none\"");
    if ((format || depth || secondary) && !render.find("outputs")) {
        json::Value outputs = json::Value::array();
        json::Value first = json::Value::object();
        first["format"] = "wav";
        if (format && format->isString()) first["format"] = *format;
        if (format && format->isObject()) {
            if (const auto* c = format->find("container")) {
                if (!c->isString()) return typeError(at("format") + "/container", "a string");
                first["format"] = *c;
            }
            if (const auto* d = format->find("bit_depth")) first["bit_depth"] = *d;
            if (const auto* d = format->find("dither")) {
                if (!d->isBool()) return typeError(at("format") + "/dither", "a boolean");
                first["dither"] = d->asBool() ? "tpdf" : "none";
            }
        }
        if (depth && !first.find("bit_depth")) first["bit_depth"] = *depth;
        outputs.push_back(std::move(first));
        if (secondary && secondary->isObject()) {
            json::Value second = json::Value::object();
            if (const auto* f = secondary->find("format")) second["format"] = *f;
            if (const auto* d = secondary->find("bit_depth")) second["bit_depth"] = *d;
            outputs.push_back(std::move(second));
        } else if (secondary && secondary->isString() && secondary->asString() != "none")
            return Error("schema.enum", "secondary must be {format, bit_depth} or \"none\"",
                         at("secondary"), "use outputs, e.g. [{\"format\":\"flac\"}]");
        members.erase("format");
        members.erase("bit_depth");
        members.erase("secondary");
        render["outputs"] = std::move(outputs);
    } else {
        members.erase("format");
        members.erase("bit_depth");
        members.erase("secondary");
    }
    if (const json::Value* tail = render.find("tail_ms")) {
        if (!tail->isNumber()) return typeError(at("tail_ms"), "a number of milliseconds");
        if (!render.find("tail_s")) render["tail_s"] = tail->asDouble() / 1000.0;
        members.erase("tail_ms");
    }
    if (json::Value* normalize = members.find("normalize")) {
        if (normalize->isString()) {
            json::Value object = json::Value::object();
            object["mode"] = *normalize;
            *normalize = std::move(object);
        } else if (normalize->isObject()) {
            if (const json::Value* limit = normalize->find("limit_true_peak_db")) {
                if (!render.find("limit_true_peak_db")) {
                    const json::Value copy = *limit;
                    normalize->asObject().erase("limit_true_peak_db");
                    render["limit_true_peak_db"] = copy;
                } else
                    normalize->asObject().erase("limit_true_peak_db");
            }
        }
    }
    return okStatus();
}

}  // namespace supple::model::render
