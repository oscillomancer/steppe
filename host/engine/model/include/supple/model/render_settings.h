// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The project's render settings, `project.render` (session-model §1.10,
// ADR-0055): every member optional, so a project, the user's default and a
// render command's arguments can each give some of them. The engine resolves
// them per top-level member (engine.md §3). Stored in the project as the
// canonical JSON of `Settings`; files before schema 2.3 are migrated by the
// schema's decoder (migrateLegacy).
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "supple/base/result.h"
#include "supple/json/json.h"
#include "supple/schema/schema.h"

namespace supple::model::render {

enum class Source { Mix, Stems, Regions };
enum class Bounds { Project, Loop, Custom };
enum class Format { Wav, Aiff, Flac, Mp3, Vorbis };
enum class Dither { None, Tpdf, Shaped };
enum class Mp3Mode { Cbr, Vbr };
enum class NormalizeMode { None, LufsI, LufsS, LufsM, Peak, TruePeak, Rms };
enum class Scope { File, Master };
enum class StemMode { Isolated, ThroughMaster };
enum class StemTap { PostFader, PreFader, PreFx };
enum class IfExists { Overwrite, Number, Skip, Error };

inline constexpr int kChannelsPerTrack = 0;  // channels: "track"
inline constexpr std::size_t kMaxOutputs = 4;

struct Mp3 {
    std::optional<Mp3Mode> mode;
    std::optional<std::int64_t> bitrateKbps, vbrQuality;
    bool operator==(const Mp3&) const = default;
};
struct Output {
    Format format = Format::Wav;
    std::optional<std::int64_t> bitDepth, sampleRate;
    std::optional<Dither> dither;
    std::optional<Mp3> mp3;
    std::optional<double> vorbisQuality;
    std::optional<std::string> suffix;
    bool operator==(const Output&) const = default;
};
struct Normalize {
    NormalizeMode mode = NormalizeMode::None;
    std::optional<double> target;
    std::optional<bool> onlyReduce;
    std::optional<Scope> scope;
    bool operator==(const Normalize&) const = default;
};
struct Trim {
    std::optional<bool> start, end;
    std::optional<double> thresholdDb;
    bool operator==(const Trim&) const = default;
};
struct Stems {
    std::optional<StemMode> mode;
    std::optional<StemTap> tap;
    std::optional<std::vector<std::string>> tracks;
    std::optional<bool> skipSilent;
    bool operator==(const Stems&) const = default;
};
struct Metadata {
    std::optional<std::string> title, artist, album, date, genre, comment, isrc, cover;
    std::optional<std::int64_t> trackNumber;
    std::optional<bool> bwf, markers;
    bool operator==(const Metadata&) const = default;
};
// A string for every source (`all`), or one per source.
struct Pattern {
    std::optional<std::string> all, mix, stems, regions;
    bool operator==(const Pattern&) const = default;
};
using Matrix = std::map<std::string, std::vector<std::string>>;

struct Settings {
    std::optional<Source> source;
    // The mix's masters, one file each (ADR-0056); absent: the main master.
    std::optional<std::vector<std::string>> masters;
    std::optional<Bounds> bounds;
    std::optional<double> startBeat, endBeat, tailS;
    std::optional<Pattern> pattern;
    std::optional<std::int64_t> sampleRate;
    std::optional<int> channels;  // 1, 2 or kChannelsPerTrack
    std::optional<std::vector<Output>> outputs;
    std::optional<Normalize> normalize;
    std::optional<double> limitTruePeakDb;
    std::optional<Trim> trim;
    std::optional<bool> seamlessLoop;
    std::optional<double> handlesS;
    std::optional<Stems> stems;
    std::optional<Matrix> matrix;
    std::optional<IfExists> ifExists;
    std::optional<Metadata> metadata;
    std::optional<bool> report, addToProject;
    bool operator==(const Settings&) const = default;
};

// The members of Settings, by their JSON names, in the spec's order.
std::span<const std::string_view> fieldNames();
// For each member: visit(name, member pointer). The engine resolves layers
// with it.
template <class F>
void forEachField(F&& visit) {
    visit("source", &Settings::source);
    visit("masters", &Settings::masters);
    visit("bounds", &Settings::bounds);
    visit("start_beat", &Settings::startBeat);
    visit("end_beat", &Settings::endBeat);
    visit("tail_s", &Settings::tailS);
    visit("pattern", &Settings::pattern);
    visit("sample_rate", &Settings::sampleRate);
    visit("channels", &Settings::channels);
    visit("outputs", &Settings::outputs);
    visit("normalize", &Settings::normalize);
    visit("limit_true_peak_db", &Settings::limitTruePeakDb);
    visit("trim", &Settings::trim);
    visit("seamless_loop", &Settings::seamlessLoop);
    visit("handles_s", &Settings::handlesS);
    visit("stems", &Settings::stems);
    visit("matrix", &Settings::matrix);
    visit("if_exists", &Settings::ifExists);
    visit("metadata", &Settings::metadata);
    visit("report", &Settings::report);
    visit("add_to_project", &Settings::addToProject);
}

schema::Schema<Settings> settingsSchema();
schema::Schema<Output> outputSchema();
schema::Schema<Normalize> normalizeSchema();
schema::Schema<Stems> stemsSchema();
schema::Schema<Pattern> patternSchema();
schema::Schema<Format> formatSchema();  // wav | aiff | flac | mp3 | vorbis
std::string_view formatName(Format format);
std::string_view extension(Format format);  // ".wav", ".aif", ".flac", ".mp3", ".ogg"
// Rewrites the members before schema 2.3 in place (format, bit_depth,
// secondary, tail_ms, normalize as a string, normalize.limit_true_peak_db);
// a value of the wrong type is a schema.type error at its path.
Status migrateLegacy(json::Value& render, const std::string& path = "");
// The canonical JSON of the settings (members absent when unset).
json::Value toJson(const Settings& settings);
// The MP3 constant bit rates LAME accepts (kbps).
std::span<const std::int64_t> mp3Bitrates();

}  // namespace supple::model::render
