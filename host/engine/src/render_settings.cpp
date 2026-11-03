// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "render_settings.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "supple/base/files.h"
#include "supple/engine/app.h"
#include "supple/json/json.h"
#include "supple/model/schemas.h"

namespace supple::engine {
namespace {
namespace fs = std::filesystem;
constexpr std::string_view kPresetSchema = "supple.render-preset/1.0";
constexpr std::size_t kMaxPresetFile = 1 << 20;
constexpr std::array<int, 9> kMp3Rates{8000,  11025, 12000, 16000, 22050,
                                       24000, 32000, 44100, 48000};

struct PresetFile {
    std::string schema, name;
    rs::Settings settings;
};
schema::Schema<PresetFile> presetFileSchema() {
    return schema::object<PresetFile>("render preset")
        .field("schema", &PresetFile::schema,
               schema::string().pattern("^supple\\.render-preset/1\\.[0-9]+$",
                                        "supple.render-preset/1.0"))
        .field("name", &PresetFile::name, schema::string())
        .field("settings", &PresetFile::settings, rs::settingsSchema())
        .build();
}

rs::Output output(rs::Format format, std::optional<std::int64_t> bits = {},
                  std::optional<rs::Dither> dither = {}) {
    rs::Output o;
    o.format = format;
    o.bitDepth = bits;
    o.dither = dither;
    return o;
}

fs::path presetPath(const EngineApp& app, const std::optional<std::string>& name) {
    return renderPresetDir(app) / ((name ? *name : std::string("default")) + ".json");
}

bool isFactory(const std::string& name) {
    for (const auto& preset : factoryRenderPresets())
        if (preset.name == name) return true;
    return false;
}

Result<rs::Settings> readPreset(const fs::path& path) {
    auto text = readTextFile(path, kMaxPresetFile);
    if (!text) return std::move(text).error();
    auto doc = json::parse(*text);
    if (!doc) return std::move(doc).error();
    schema::DecodeOptions options;
    options.unknownMembers = schema::UnknownMembers::Ignore;  // newer minor versions
    if (doc->isObject())
        if (auto* settings = doc->asObject().find("settings"))
            SUPPLE_TRY(rs::migrateLegacy(*settings, "/settings"));
    auto decoded = presetFileSchema().decode(*doc, options);
    if (!decoded) {
        auto error = std::move(decoded).error();
        error.hint = path.string() + ": " + error.hint;
        return error;
    }
    return presetSettings(std::move(decoded->settings));
}
}  // namespace

rs::Settings projectRenderSettings(const model::Project& project) {
    schema::DecodeOptions options;
    options.unknownMembers = schema::UnknownMembers::Ignore;
    auto decoded = rs::settingsSchema().decode(project.render, options);
    return decoded ? std::move(*decoded) : rs::Settings{};
}

rs::Settings builtInRenderSettings(const model::Session& s) {
    rs::Settings b;
    b.source = rs::Source::Mix;
    b.bounds = rs::Bounds::Project;
    b.tailS = 0.0;
    rs::Pattern pattern;
    pattern.mix = "{project}-v{version}";
    pattern.stems = "{project}-{track}-{region}";
    pattern.regions = "{project}-{track}-{region}";
    b.pattern = pattern;
    b.sampleRate = static_cast<std::int64_t>(std::llround(s.project->sampleRate));
    b.channels = 2;
    b.outputs = std::vector<rs::Output>{output(rs::Format::Wav, 32)};
    rs::Normalize normalize;
    normalize.target = -14.0;
    b.normalize = normalize;
    rs::Trim trim;
    trim.start = false;
    trim.end = false;
    trim.thresholdDb = -60.0;
    b.trim = trim;
    b.seamlessLoop = false;
    b.handlesS = 0.0;
    rs::Stems stems;
    stems.mode = rs::StemMode::Isolated;
    stems.tap = rs::StemTap::PostFader;
    stems.skipSilent = false;
    b.stems = stems;
    b.ifExists = rs::IfExists::Overwrite;
    b.report = false;
    b.addToProject = false;
    return b;
}

rs::Settings resolveRenderSettings(const rs::Settings& args, const rs::Settings& project,
                                   const rs::Settings& user, const rs::Settings& builtIn,
                                   RenderOrigins* from) {
    rs::Settings out;
    rs::forEachField([&](std::string_view name, auto member) {
        const char* layer = "built-in";
        if (args.*member)
            out.*member = args.*member, layer = "args";
        else if (project.*member)
            out.*member = project.*member, layer = "project";
        else if (user.*member)
            out.*member = user.*member, layer = "default";
        else
            out.*member = builtIn.*member;
        if (from) (*from)[std::string(name)] = layer;
    });
    return out;
}

int defaultBitDepth(rs::Format format) {
    switch (format) {
        case rs::Format::Wav:
            return 32;
        case rs::Format::Aiff:
        case rs::Format::Flac:
            return 24;
        case rs::Format::Mp3:
        case rs::Format::Vorbis:
            return 0;
    }
    return 32;
}

int outputSampleRate(const rs::Output& output, int chainRate) {
    if (output.sampleRate) return static_cast<int>(*output.sampleRate);
    if (output.format == rs::Format::Mp3) {
        if (std::find(kMp3Rates.begin(), kMp3Rates.end(), chainRate) != kMp3Rates.end())
            return chainRate;
        if (chainRate > 48000) return chainRate % 44100 == 0 ? 44100 : 48000;
        for (int rate : kMp3Rates)
            if (rate >= chainRate) return rate;
        return 48000;
    }
    if (output.format == rs::Format::Vorbis && chainRate > 192000) return chainRate / 2;
    return chainRate;
}

void completeRenderSettings(rs::Settings& s) {
    if (s.pattern && s.pattern->all) {
        const std::string all = *s.pattern->all;
        s.pattern = rs::Pattern{{}, all, all, all};
    } else if (s.pattern) {
        if (!s.pattern->mix) s.pattern->mix = "{project}-v{version}";
        if (!s.pattern->stems) s.pattern->stems = "{project}-{track}-{region}";
        if (!s.pattern->regions) s.pattern->regions = "{project}-{track}-{region}";
    }
    if (s.outputs)
        for (auto& o : *s.outputs) {
            const int bits = defaultBitDepth(o.format);
            if (bits && !o.bitDepth) o.bitDepth = bits;
            // Dither applies to 16- and 24-bit output only; a 32-bit output
            // leaves it unset, so a later change of depth gets the default.
            if ((o.bitDepth == 16 || o.bitDepth == 24) && !o.dither) o.dither = rs::Dither::Tpdf;
            if (o.format == rs::Format::Mp3) {
                rs::Mp3 mp3 = o.mp3.value_or(rs::Mp3{});
                if (!mp3.mode) mp3.mode = rs::Mp3Mode::Cbr;
                if (*mp3.mode == rs::Mp3Mode::Cbr && !mp3.bitrateKbps) mp3.bitrateKbps = 320;
                if (*mp3.mode == rs::Mp3Mode::Vbr && !mp3.vbrQuality) mp3.vbrQuality = 2;
                o.mp3 = mp3;
            }
            if (o.format == rs::Format::Vorbis && !o.vorbisQuality) o.vorbisQuality = 6.0;
        }
    if (s.normalize) {
        if (!s.normalize->target) s.normalize->target = -14.0;
        if (!s.normalize->onlyReduce) s.normalize->onlyReduce = false;
        if (!s.normalize->scope) s.normalize->scope = rs::Scope::File;
    }
    if (s.trim) {
        if (!s.trim->start) s.trim->start = false;
        if (!s.trim->end) s.trim->end = false;
        if (!s.trim->thresholdDb) s.trim->thresholdDb = -60.0;
    }
    if (s.stems) {
        if (!s.stems->mode) s.stems->mode = rs::StemMode::Isolated;
        if (!s.stems->tap) s.stems->tap = rs::StemTap::PostFader;
        if (!s.stems->skipSilent) s.stems->skipSilent = false;
    }
}

fs::path renderPresetDir(const EngineApp& app) {
    return app.options().renderPresetDir.empty() ? app.options().workdir / "render-presets"
                                                 : app.options().renderPresetDir;
}

Result<std::optional<rs::Settings>> loadRenderDefault(const EngineApp& app) {
    const fs::path path = presetPath(app, std::nullopt);
    std::error_code ec;
    if (!fs::exists(path, ec)) return std::optional<rs::Settings>{};
    auto settings = readPreset(path);
    if (!settings) return std::move(settings).error();
    return std::optional<rs::Settings>(std::move(*settings));
}

std::vector<RenderPreset> factoryRenderPresets() {
    std::vector<RenderPreset> list;
    auto add = [&](std::string name, rs::Settings s) {
        list.push_back({std::move(name), true, std::move(s)});
    };
    {
        rs::Settings s;
        s.outputs = std::vector<rs::Output>{output(rs::Format::Wav, 24, rs::Dither::Tpdf)};
        add("WAV 24-bit", s);
    }
    {
        rs::Settings s;
        s.outputs = std::vector<rs::Output>{output(rs::Format::Wav, 24, rs::Dither::Tpdf)};
        rs::Normalize n;
        n.mode = rs::NormalizeMode::LufsI;
        n.target = -14.0;
        s.normalize = n;
        s.limitTruePeakDb = -1.0;
        s.report = true;
        add("Streaming master (−14 LUFS, −1 dBTP)", s);
    }
    {
        rs::Settings s;
        s.sampleRate = 44100;
        s.outputs = std::vector<rs::Output>{output(rs::Format::Wav, 16, rs::Dither::Shaped)};
        s.limitTruePeakDb = -1.0;
        add("CD master (16-bit 44.1 kHz, −1 dBTP)", s);
    }
    {
        rs::Settings s;
        rs::Output mp3 = output(rs::Format::Mp3);
        mp3.mp3 = rs::Mp3{rs::Mp3Mode::Cbr, 128, {}};
        s.outputs = std::vector<rs::Output>{mp3};
        s.channels = 1;
        rs::Normalize n;
        n.mode = rs::NormalizeMode::LufsI;
        n.target = -16.0;
        s.normalize = n;
        s.limitTruePeakDb = -1.0;
        add("Podcast (MP3, −16 LUFS)", s);
    }
    {
        rs::Settings s;
        s.source = rs::Source::Stems;
        s.outputs = std::vector<rs::Output>{output(rs::Format::Wav, 24, rs::Dither::Tpdf)};
        s.channels = rs::kChannelsPerTrack;
        rs::Stems stems;
        stems.mode = rs::StemMode::Isolated;
        stems.tap = rs::StemTap::PostFader;
        s.stems = stems;
        add("Stems for mixing (24-bit)", s);
    }
    {
        rs::Settings s;
        rs::Output ogg = output(rs::Format::Vorbis);
        ogg.vorbisQuality = 6.0;
        s.outputs = std::vector<rs::Output>{ogg};
        s.bounds = rs::Bounds::Loop;
        s.tailS = 2.0;
        s.seamlessLoop = true;
        add("Game loop (Ogg Vorbis, seamless)", s);
    }
    return list;
}

rs::Settings presetSettings(rs::Settings s) {
    if (s.stems) s.stems->tracks.reset();
    s.matrix.reset();
    s.startBeat.reset();
    s.endBeat.reset();
    if (s.bounds == rs::Bounds::Custom) s.bounds.reset();
    return s;
}

Status validPresetName(const std::string& name) {
    static constexpr std::string_view kRefused = "<>:\"/\\|?*";
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name.empty() || name.size() > 120 || name.front() == '.' || name.back() == '.' ||
        name.back() == ' ' || name.front() == ' ' || lower == "default" ||
        std::any_of(name.begin(), name.end(), [](unsigned char c) {
            return c < 32 || c == 127 || kRefused.find(static_cast<char>(c)) != kRefused.npos;
        }))
        return Error("render.preset_name", "invalid preset name \"" + name + "\"", "/name",
                     "use 1-120 characters without < > : \" / \\ | ? *, not starting or ending "
                     "with a dot or space; \"default\" is the user default (default: true)");
    return okStatus();
}

Result<std::vector<RenderPreset>> listRenderPresets(const EngineApp& app) {
    auto list = factoryRenderPresets();
    const fs::path dir = renderPresetDir(app);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return list;
    std::vector<RenderPreset> user;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().extension() != ".json") continue;
        const std::string name = entry.path().stem().string();
        if (!validPresetName(name) || isFactory(name)) continue;
        auto settings = readPreset(entry.path());
        if (!settings) continue;  // a damaged file is left out, not fatal
        user.push_back({name, false, std::move(*settings)});
    }
    std::sort(user.begin(), user.end(),
              [](const RenderPreset& a, const RenderPreset& b) { return a.name < b.name; });
    for (auto& preset : user) list.push_back(std::move(preset));
    return list;
}

Status saveRenderPreset(const EngineApp& app, const std::optional<std::string>& name,
                        const rs::Settings& settings) {
    if (name) {
        SUPPLE_TRY(validPresetName(*name));
        if (isFactory(*name))
            return Error("render.preset_factory", "factory presets cannot be overwritten", "/name",
                         "save under another name");
    }
    PresetFile file{std::string(kPresetSchema), name.value_or("default"), presetSettings(settings)};
    const fs::path path = presetPath(app, name);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec)
        return Error("render.preset_write", "cannot create " + path.parent_path().string(), "",
                     "check that the user data folder is writable");
    return writeFileAtomic(path, json::writeCanonical(presetFileSchema().encode(file)) + "\n");
}

Status deleteRenderPreset(const EngineApp& app, const std::optional<std::string>& name) {
    if (name) {
        if (isFactory(*name))
            return Error("render.preset_factory", "factory presets cannot be deleted", "/name",
                         "delete one of your own presets (render.preset.list)");
        SUPPLE_TRY(validPresetName(*name));
    }
    const fs::path path = presetPath(app, name);
    std::error_code ec;
    if (!fs::remove(path, ec))
        return Error("render.preset_unknown",
                     name ? "no preset \"" + *name + "\"" : std::string("no user default saved"),
                     name ? "/name" : "/default", "render.preset.list shows the presets");
    return okStatus();
}

Result<rs::Settings> findRenderPreset(const EngineApp& app, const std::string& name) {
    for (const auto& preset : factoryRenderPresets())
        if (preset.name == name) return preset.settings;
    SUPPLE_TRY(validPresetName(name));
    const fs::path path = presetPath(app, name);
    std::error_code ec;
    if (!fs::exists(path, ec))
        return Error("render.preset_unknown", "no preset \"" + name + "\"", "/name",
                     "render.preset.list shows the presets");
    return readPreset(path);
}

}  // namespace supple::engine
