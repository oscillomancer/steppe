// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "render_commands.h"
#include "supple/model/numeric.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <span>
#include <sstream>

#include <supple/product.h>
#include <tuple>
#include "../../analysis-commands/analysis_shared.h"
#include "analysis_commands.h"
#include "render_settings.h"
#include "supple/base/files.h"
#include "supple/engine/app.h"
#include "supple/time/timeline.h"

namespace supple::engine {
namespace {
using cmd::Command;
using cmd::Context;
using cmd::Kind;
namespace fs = std::filesystem;

struct AbArgs {
    std::string a, b;
    std::optional<double> startBeat, endBeat, tailS;
};
using PatternData = RenderPatternContext;

json::Value obj(std::string_view s) { return json::parse(s).value(); }
std::string pad(std::int64_t n, int width) {
    std::ostringstream out;
    out << std::setw(width) << std::setfill('0') << n;
    return out.str();
}
std::string compact(double v) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << v;
    std::string s = out.str();
    while (s.ends_with('0')) s.pop_back();
    if (s.ends_with('.')) s.pop_back();
    return s;
}
Result<std::string> token(std::string_view name, const PatternData& d) {
    auto colon = name.find(':');
    const std::string key(name.substr(0, colon));
    const std::string arg =
        colon == std::string_view::npos ? "" : std::string(name.substr(colon + 1));
    if (key == "project" && arg.empty()) return d.project;
    if (key == "track" && arg.empty()) return d.track;
    if (key == "folder_path" && arg.empty()) return d.folderPath;
    if (key == "region" && arg.empty()) return d.region;
    if (key == "marker" && arg.empty()) return d.marker;
    if (key == "item" && arg.empty()) return d.item;
    if (key == "take" && arg.empty()) return d.take;
    if (key == "format" && arg.empty()) return d.format;
    if (key == "sample_rate" && arg.empty()) return std::to_string(d.sampleRate);
    if (key == "bit_depth" && arg.empty())
        return d.bitDepth ? std::to_string(d.bitDepth) : std::string();
    if (key == "version" && arg.empty()) return std::to_string(d.version);
    // Branch names may hold "/" (agent/claude/darker): one folder level, not several.
    if (key == "branch" && arg.empty()) {
        std::string folder = d.branch;
        std::replace(folder.begin(), folder.end(), '/', '-');
        return folder;
    }
    if (key == "track_number" || key == "region_number" || key == "counter") {
        if (!arg.empty() &&
            (arg.size() > 3 || arg[0] != '0' ||
             !std::all_of(arg.begin(), arg.end(), [](unsigned char c) { return std::isdigit(c); })))
            return Error("render.pattern", "invalid number format", "/pattern", "use :02 or :03");
        int width = 1;
        if (!arg.empty()) {
            width = 0;
            for (char c : arg) width = width * 10 + (c - '0');
        }
        return pad(key == "track_number"    ? d.trackNumber
                   : key == "region_number" ? d.regionNumber
                                            : d.counter,
                   width);
    }
    if (key == "start") {
        if (arg == "beats") return compact(d.startBeats);
        if (arg == "seconds") return compact(d.startSeconds);
        if (arg == "timecode") {
            if (!model::checkedSamples(d.startSeconds, 1, "/start_beat") || d.startSeconds < 0)
                return model::numericError("/start_beat");
            const auto sec = static_cast<std::int64_t>(d.startSeconds);
            return pad(sec / 3600, 2) + "-" + pad((sec / 60) % 60, 2) + "-" + pad(sec % 60, 2);
        }
    }
    if (key == "date" && arg == "YYYY-MM-DD") {
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm t{};
#if defined(_WIN32)
        gmtime_s(&t, &now);
#else
        gmtime_r(&now, &t);
#endif
        char buf[16];
        std::strftime(buf, sizeof buf, "%Y-%m-%d", &t);
        return std::string(buf);
    }
    if (key == "time" && arg == "HH-mm-ss") {
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm t{};
#if defined(_WIN32)
        gmtime_s(&t, &now);
#else
        gmtime_r(&now, &t);
#endif
        char buf[16];
        std::strftime(buf, sizeof buf, "%H-%M-%S", &t);
        return std::string(buf);
    }
    return Error("render.pattern", "unknown or invalid token {" + std::string(name) + "}",
                 "/pattern", "use the closed token set in session-model §7.3");
}
// Pattern arguments are literal quoted strings. Delimiters inside them do
// not end a token or start another filter; the language has no quote escapes.
std::size_t unquotedDelimiter(std::string_view text, char delimiter, std::size_t from = 0) {
    bool quoted = false;
    for (std::size_t i = from; i < text.size(); ++i) {
        if (text[i] == '\"')
            quoted = !quoted;
        else if (!quoted && text[i] == delimiter)
            return i;
    }
    return std::string_view::npos;
}

// Tokens and filters only (metadata text); expand() adds the file-name checks.
Result<std::string> expandText(std::string_view pattern, const PatternData& d) {
    std::string out;
    for (std::size_t i = 0; i < pattern.size();) {
        if (pattern[i] != '{') {
            if (pattern[i] == '}')
                return Error("render.pattern", "unmatched }", "/pattern",
                             "remove the unmatched } from pattern or include it inside a quoted "
                             "replace argument");
            out += pattern[i++];
            continue;
        }
        auto end = unquotedDelimiter(pattern, '}', i + 1);
        if (end == std::string_view::npos)
            return Error("render.pattern", "unclosed {", "/pattern",
                         "close each token with } and each replace argument with a double quote");
        std::string inside(pattern.substr(i + 1, end - i - 1));
        auto pipe = unquotedDelimiter(inside, '|');
        auto value = token(std::string_view(inside).substr(0, pipe), d);
        if (!value) return std::move(value).error();
        while (pipe != std::string::npos) {
            auto next = unquotedDelimiter(inside, '|', pipe + 1);
            std::string filter =
                inside.substr(pipe + 1, next == std::string::npos ? next : next - pipe - 1);
            if (filter == "lower")
                std::transform(value->begin(), value->end(), value->begin(),
                               [](unsigned char c) { return std::tolower(c); });
            else if (filter == "upper")
                std::transform(value->begin(), value->end(), value->begin(),
                               [](unsigned char c) { return std::toupper(c); });
            else if (filter.starts_with("max(") && filter.ends_with(')')) {
                const std::string n = filter.substr(4, filter.size() - 5);
                std::size_t maximum = 0;
                const auto parsed = std::from_chars(n.data(), n.data() + n.size(), maximum);
                if (n.empty() || parsed.ec != std::errc{} || parsed.ptr != n.data() + n.size())
                    return Error("render.pattern", "invalid max filter", "/pattern",
                                 "use max with a nonnegative integer, for example {track|max(8)}");
                value->resize(std::min(value->size(), maximum));
            } else if (filter.starts_with("replace(\"") && filter.ends_with("\")")) {
                const auto mid = filter.find('"', 9);
                if (mid == std::string::npos || filter.substr(mid, 3) != "\",\"" ||
                    filter.find('"', mid + 3) != filter.size() - 2)
                    return Error(
                        "render.pattern", "invalid replace filter", "/pattern",
                        "use replace(\"from\",\"to\") with two balanced literal quoted strings");
                const std::string from = filter.substr(9, mid - 9),
                                  to = filter.substr(mid + 3, filter.size() - mid - 5);
                if (from.empty())
                    return Error("render.pattern", "empty replace source", "/pattern",
                                 "provide a nonempty first argument to replace, for example "
                                 "replace(\" \",\"_\")");
                for (std::size_t at = 0; (at = value->find(from, at)) != std::string::npos;
                     at += to.size())
                    value->replace(at, from.size(), to);
            } else
                return Error("render.pattern", "unknown filter " + filter, "/pattern",
                             "use balanced token braces and literal quoted replace arguments with "
                             "the filters lower, upper, max or replace");
            pipe = next;
        }
        bool removedSeparator = false;
        if (value->empty() && !out.empty() &&
            std::string("-_. ").find(out.back()) != std::string::npos) {
            out.pop_back();
            removedSeparator = true;
        } else
            out += *value;
        i = end + 1;
        if (value->empty() && !removedSeparator && i < pattern.size() &&
            std::string("-_. ").find(pattern[i]) != std::string::npos)
            ++i;
    }
    return out;
}

Result<std::string> expand(std::string_view pattern, const PatternData& d) {
    auto expanded = expandText(pattern, d);
    if (!expanded) return std::move(expanded).error();
    const std::string& out = *expanded;
    if (out.find('\0') != std::string::npos)
        return Error("render.path", "render name contains a NUL byte", "/pattern",
                     "remove NUL bytes from the pattern and its token values");
    if (out.empty() || out == "." || out == ".." || fs::path(out).is_absolute() ||
        out.find("..") != std::string::npos || out.find('\\') != std::string::npos)
        return Error("render.path", "render name escapes the renders directory", "/pattern",
                     "use a relative name without ..");
    return out;
}

// ------------------------------------------------------------------ arguments

enum class RenderAnalyze { Summary, Full };

// Per-call selectors and the arguments kept from before ADR-0055; every
// other argument is a render setting (rs::Settings).
struct Selectors {
    std::optional<std::string> master, branch, name;
    std::optional<std::int64_t> version;
    std::optional<rs::Source> operation;
    std::optional<std::vector<std::string>> regions, tracks;
    std::optional<rs::Format> format;
    std::optional<std::int64_t> bitDepth;
    std::optional<json::Value> secondary;
    std::optional<rs::StemMode> mode;
    std::optional<rs::StemTap> tap;
    std::optional<double> target;
    std::optional<RenderAnalyze> analyze;
    bool job = false;
};
struct Args {
    Selectors sel;
    rs::Settings settings;
};

schema::Schema<Selectors> selectorSchema() {
    return schema::object<Selectors>("render selectors")
        .field("master", &Selectors::master,
               schema::string()
                   .pattern("^tr_[0-9]+$", "master id")
                   .doc("Mix: one master for this call (default: the settings' masters, "
                        "else tr_0)"))
        .field("branch", &Selectors::branch,
               schema::string().doc("Render this branch's head (default: the current one)"))
        .field("version", &Selectors::version,
               schema::integer().min(0).doc("Render the session as of this version "
                                            "(history.list); the render queue pins its renders"))
        .field("name", &Selectors::name,
               schema::string().doc("A file name instead of the pattern, for this call"))
        .field("operation", &Selectors::operation,
               schema::enumeration<rs::Source>({{rs::Source::Mix, "mix"},
                                                {rs::Source::Stems, "stems"},
                                                {rs::Source::Regions, "regions"}})
                   .doc("Dry run and render.settings: mix, stems or regions (default: tracks or "
                        "regions given, else the settings' source)"))
        .field("regions", &Selectors::regions,
               schema::arrayOf(schema::string().pattern("^rg_[0-9]+$", "region id"))
                   .doc("Regions: region ids (default: all, with the render matrix)"))
        .field("tracks", &Selectors::tracks,
               schema::arrayOf(schema::string().pattern("^tr_[0-9]+$", "track id"))
                   .doc("Stems: track ids (sets stems.tracks for this call)"))
        .field("format", &Selectors::format,
               rs::formatSchema().doc("Sets outputs[0].format for this call"))
        .field("bit_depth", &Selectors::bitDepth,
               schema::integer().range(16, 32).doc("Sets outputs[0].bit_depth for this call"))
        .field("secondary", &Selectors::secondary,
               schema::any().doc("Replaces outputs[1] ({format, bit_depth}), or \"none\" removes "
                                 "it, for this call"))
        .field("mode", &Selectors::mode,
               schema::enumeration<rs::StemMode>({{rs::StemMode::Isolated, "isolated"},
                                                  {rs::StemMode::ThroughMaster, "through_master"}})
                   .doc("Sets stems.mode for this call"))
        .field("tap", &Selectors::tap,
               schema::enumeration<rs::StemTap>({{rs::StemTap::PostFader, "post_fader"},
                                                 {rs::StemTap::PreFader, "pre_fader"},
                                                 {rs::StemTap::PreFx, "pre_fx"}})
                   .doc("Sets stems.tap for this call"))
        .field("target", &Selectors::target,
               schema::number().doc("Sets normalize.target for this call"))
        .field("analyze", &Selectors::analyze,
               schema::enumeration<RenderAnalyze>(
                   {{RenderAnalyze::Summary, "summary"}, {RenderAnalyze::Full, "full"}})
                   .doc("Mix: a supple.analysis-report/1.0 inline"))
        .field("job", &Selectors::job, schema::boolean())
        .defaultValue(false)
        .omitIfDefault()
        .build();
}

// The selectors and every render setting in one flat object (engine.md §3).
schema::Schema<Args> argsSchema() {
    static const schema::Schema<Args> s = [] {
        const auto selectors = selectorSchema();
        const auto settings = rs::settingsSchema();
        std::set<std::string> selectorNames, settingNames;
        json::Value spec = json::Value::object();
        spec["type"] = "object";
        spec["title"] = "render_args";
        spec["properties"] = json::Value::object();
        for (const auto& [names, exported] : {std::pair{&settingNames, settings.jsonSchema()},
                                              std::pair{&selectorNames, selectors.jsonSchema()}})
            for (const auto& p : exported.find("properties")->asObject()) {
                names->insert(p.key);
                spec["properties"][p.key] = p.value;
            }
        spec["additionalProperties"] = false;
        auto decode = [selectors, settings, selectorNames, settingNames](
                          const json::Value& v, Args& out, schema::DecodeCtx& c,
                          const std::string& p) {
            if (!v.isObject()) {
                schema::wrongType(c, p, "object");
                return false;
            }
            json::Value a = json::Value::object(), b = json::Value::object();
            bool ok = true;
            for (const auto& m : v.asObject()) {
                if (selectorNames.count(m.key))
                    a[m.key] = m.value;
                else if (settingNames.count(m.key))
                    b[m.key] = m.value;
                else {
                    c.error("schema.unknown_member", "unknown member \"" + m.key + "\"",
                            json::pointerJoin(p, m.key),
                            "use the render settings (session-model §1.10) or the selectors "
                            "branch, version, master, name, operation, regions, tracks, job");
                    ok = false;
                }
            }
            if (auto migrated = rs::migrateLegacy(b, p); !migrated) {
                c.error(migrated.error().code, migrated.error().message, migrated.error().path,
                        migrated.error().hint);
                return false;
            }
            ok &= selectors.decodeInto(a, out.sel, c, p);
            ok &= settings.decodeInto(b, out.settings, c, p);
            return ok;
        };
        auto encode = [selectors, settings](const Args& a) {
            json::Value out = settings.encode(a.settings);
            const json::Value chosen = selectors.encode(a.sel);
            for (const auto& m : chosen.asObject()) out[m.key] = m.value;
            return out;
        };
        return schema::Schema<Args>("render arguments", decode, encode, spec);
    }();
    return s;
}

Error argsError(std::string message, std::string path, std::string hint) {
    return Error("render.args", std::move(message), std::move(path), std::move(hint));
}

// ------------------------------------------------------------------ resolution

Result<model::Session> sessionFor(EngineApp& app, const std::optional<std::string>& branch,
                                  std::optional<std::int64_t> version = std::nullopt) {
    if (version) {
        if (branch)
            return Error("render.source", "give branch or version, not both", "/version",
                         "choose either branch or version and omit the other selector");
        const model::Version* v = app.versions().version(static_cast<std::uint64_t>(*version));
        if (!v || !v->session)
            return Error("history.version", "no version " + std::to_string(*version), "/version",
                         "history.list shows the versions");
        return *v->session;
    }
    if (!branch) return app.view();
    auto head = app.versions().head(*branch);
    if (!head)
        return Error("branch.unknown", "unknown branch " + *branch, "/branch", "use branch.list");
    return *head;
}

struct Resolved {
    rs::Settings effective;
    RenderOrigins from;
    std::optional<rs::Settings> userDefault;
};

std::string layerNote(const RenderOrigins& from, const std::string& field) {
    const auto it = from.find(field);
    if (it == from.end() || it->second == "args") return "";
    return it->second == "project"   ? " (from the project's render settings)"
           : it->second == "default" ? " (from your render default)"
                                     : "";
}

// The contradictions engine.md §3 lists, for one operation.
Status checkSettings(const rs::Settings& e, rs::Source operation, const RenderOrigins& from) {
    const auto& outputs = *e.outputs;
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        const auto& o = outputs[i];
        const std::string at = "/outputs/" + std::to_string(i);
        const auto bits = o.bitDepth.value_or(0);
        const bool lossy = o.format == rs::Format::Mp3 || o.format == rs::Format::Vorbis;
        if (lossy && o.bitDepth)
            return argsError(std::string(rs::formatName(o.format)) + " has no bit depth" +
                                 layerNote(from, "outputs"),
                             at + "/bit_depth", "remove bit_depth from this output");
        if ((o.format == rs::Format::Aiff || o.format == rs::Format::Flac) && bits == 32)
            return argsError(std::string(rs::formatName(o.format)) + " is 16 or 24 bit" +
                                 layerNote(from, "outputs"),
                             at + "/bit_depth", "use 16 or 24 (32-bit float is WAV only)");
        const auto rate = outputSampleRate(o, static_cast<int>(*e.sampleRate));
        if (o.format == rs::Format::Mp3) {
            static constexpr std::array<std::int64_t, 9> kRates{8000,  11025, 12000, 16000, 22050,
                                                                24000, 32000, 44100, 48000};
            if (std::find(kRates.begin(), kRates.end(), rate) == kRates.end())
                return argsError(
                    "MP3 cannot be " + std::to_string(rate) + " Hz" + layerNote(from, "outputs"),
                    at + "/sample_rate",
                    "use 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100 or "
                    "48000, or leave sample_rate out");
        }
        if (o.format == rs::Format::Vorbis && rate > 192000)
            return argsError("Ogg Vorbis is at most 192000 Hz" + layerNote(from, "outputs"),
                             at + "/sample_rate", "use a lower sample_rate or leave it out");
    }
    const bool trims = e.trim && (*e.trim->start || *e.trim->end);
    if (*e.seamlessLoop) {
        if (!(*e.tailS > 0))
            return argsError(
                "a seamless loop needs a tail to fold" + layerNote(from, "seamless_loop"),
                "/seamless_loop", "set tail_s above 0, e.g. 2");
        if (trims)
            return argsError("a seamless loop cannot be trimmed" + layerNote(from, "trim"), "/trim",
                             "turn trim.start and trim.end off for seamless loops");
        if (operation == rs::Source::Regions && *e.handlesS > 0)
            return argsError("a seamless loop cannot have handles" + layerNote(from, "handles_s"),
                             "/handles_s", "set handles_s to 0 for seamless loops");
    }
    if (operation == rs::Source::Stems && *e.normalize->scope == rs::Scope::Master &&
        e.limitTruePeakDb)
        return argsError("stems that share the master's gain cannot be limited one by one" +
                             layerNote(from, "limit_true_peak_db"),
                         "/limit_true_peak_db",
                         "remove limit_true_peak_db, or normalize each stem (scope: file)");
    return okStatus();
}

Result<Resolved> resolveFor(EngineApp& app, const model::Session& s, const Args& a) {
    Resolved r;
    rs::Settings args = a.settings;
    if ((args.startBeat || args.endBeat) && !args.bounds) args.bounds = rs::Bounds::Custom;
    auto user = loadRenderDefault(app);
    if (!user) {
        auto error = std::move(user).error();
        error.hint = "fix or delete the render default (render.preset.delete {default: true}): " +
                     error.hint;
        return error;
    }
    r.userDefault = *user;
    r.effective =
        resolveRenderSettings(args, projectRenderSettings(*s.project),
                              user->value_or(rs::Settings{}), builtInRenderSettings(s), &r.from);
    auto& e = r.effective;
    const auto& sel = a.sel;
    if (args.outputs && (sel.format || sel.bitDepth || sel.secondary))
        return argsError("give outputs or format/bit_depth/secondary, not both", "/outputs",
                         "describe every file in outputs, e.g. [{\"format\":\"flac\"}]");
    if (sel.format || sel.bitDepth) {
        auto& first = e.outputs->front();
        if (sel.format && *sel.format != first.format) {
            rs::Output changed;
            changed.format = *sel.format;
            first = changed;
        }
        if (sel.bitDepth) first.bitDepth = *sel.bitDepth;
        r.from["outputs"] = "args";
    }
    if (sel.secondary) {
        if (sel.secondary->isString() && sel.secondary->asString() == "none")
            e.outputs->resize(1);
        else {
            schema::DecodeCtx ctx;
            rs::Output second;
            if (!sel.secondary->isObject() ||
                !rs::outputSchema().decodeInto(*sel.secondary, second, ctx, "/secondary")) {
                if (!ctx.errors.empty()) return ctx.errors.front();
                return Error("render.secondary",
                             "secondary must be {format, bit_depth} or \"none\"", "/secondary",
                             "e.g. {\"format\": \"flac\", \"bit_depth\": 16}");
            }
            e.outputs->resize(1);
            e.outputs->push_back(second);
        }
        r.from["outputs"] = "args";
    }
    if (sel.mode || sel.tap || sel.tracks) {
        if (sel.mode) e.stems->mode = sel.mode;
        if (sel.tap) e.stems->tap = sel.tap;
        if (sel.tracks) e.stems->tracks = sel.tracks;
        r.from["stems"] = "args";
    }
    if (sel.target && !(args.normalize && args.normalize->target)) {
        e.normalize->target = sel.target;
        r.from["normalize"] = "args";
    }
    completeRenderSettings(e);
    return r;
}

json::Value originsJson(const RenderOrigins& from) {
    json::Value out = json::Value::object();
    for (const auto name : rs::fieldNames())
        if (auto it = from.find(std::string(name)); it != from.end()) out[name] = it->second;
    return out;
}

// ------------------------------------------------------------------ planning

struct OutputFile {
    fs::path path;
    RenderEncoding encoding;
};
struct RenderTask {
    model::Session session;
    std::shared_ptr<const model::Session> pin;
    OfflineSettings settings;
    std::vector<OutputFile> outputs;
    std::string label;
    std::optional<model::Id> track, region;
    int channels = 2;
    bool exists = false;   // a destination was there when planned
    audio::FileTags tags;  // metadata text with the file's tokens expanded
};
struct RenderPlan {
    rs::Source operation = rs::Source::Mix;
    rs::Settings settings;  // effective, complete
    std::string branch;
    std::uint64_t version = 0;
    std::vector<RenderTask> tasks;
    json::Value skipped = json::Value::array();
    // Through-master normalization (normalize.scope master, stems): the
    // master mix over the stems' range.
    std::optional<RenderTask> masterGain;
    std::optional<RenderAnalyze> analyze;
    std::vector<std::uint8_t> cover;
    std::string coverMime;
};

// metadata.cover: a PNG or JPEG of at most 10 MiB inside the project folder.
// A dry run (the dialog's preview, the File menu's label) checks the file's
// size and first bytes only; a render reads it whole.
Status readCover(EngineApp& app, const std::string& cover, RenderPlan& out, bool whole) {
    const fs::path folder = app.projectPath().empty() ? fs::path() : app.projectDir();
    if (folder.empty())
        return argsError("a cover needs a saved project", "/metadata/cover",
                         "save the project, then give the cover relative to its folder");
    const fs::path rel = fs::path(cover).lexically_normal();
    if (rel.empty() || rel.is_absolute() || *rel.begin() == "..")
        return argsError("the cover must be inside the project folder", "/metadata/cover",
                         "copy the image into the project folder and name it relative to it");
    std::error_code ec;
    const auto root = fs::weakly_canonical(folder, ec);
    const auto resolved = fs::weakly_canonical(folder / rel, ec);
    const auto inside = resolved.lexically_relative(root);
    if (ec || inside.empty() || *inside.begin() == "..")
        return argsError("the cover must be inside the project folder", "/metadata/cover",
                         "copy the image into the project folder and name it relative to it");
    const auto size = fs::file_size(resolved, ec);
    if (ec || size > (10u << 20))
        return argsError("cannot read the cover " + rel.generic_string(), "/metadata/cover",
                         "give a PNG or JPEG of at most 10 MiB in the project folder");
    std::vector<std::uint8_t> head(8, 0);
    {
        std::ifstream in(resolved, std::ios::binary);
        in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
        if (!in && !in.eof())
            return argsError("cannot read the cover " + rel.generic_string(), "/metadata/cover",
                             "give a PNG or JPEG of at most 10 MiB in the project folder");
        head.resize(static_cast<std::size_t>(in.gcount()));
    }
    const auto& b = head;
    if (b.size() >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G')
        out.coverMime = "image/png";
    else if (b.size() >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)
        out.coverMime = "image/jpeg";
    else
        return argsError("the cover is not a PNG or JPEG", "/metadata/cover",
                         "give a PNG or JPEG image");
    if (!whole) return okStatus();
    auto bytes = readFile(resolved, 10u << 20);
    if (!bytes)
        return argsError("cannot read the cover " + rel.generic_string(), "/metadata/cover",
                         "give a PNG or JPEG of at most 10 MiB in the project folder");
    out.cover = std::move(*bytes);
    return okStatus();
}

Result<model::Id> trackId(const model::Session& s, const std::string& text, std::string path) {
    auto id = model::Id::parse(text);
    if (!id || !s.tracks.contains(*id))
        return Error("track.unknown", "no track " + text, std::move(path), "use track.list");
    return *id;
}
Result<model::Id> regionId(const model::Session& s, const std::string& text) {
    auto id = model::Id::parse(text);
    if (!id || !s.regions.contains(*id))
        return Error("region.unknown", "no region " + text, "/regions", "use region.list");
    return *id;
}

double lastBeat(const model::Session& s) { return model::lastItemBeat(s); }

std::uint64_t outputSeed(std::uint64_t seed, std::size_t index) {
    if (index == 0) return seed;  // the first output keeps the project's dither
    std::uint64_t z = seed + 0x9E3779B97F4A7C15ull * index;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

RenderEncoding encodingOf(const rs::Output& o, int chainRate, std::uint64_t seed,
                          std::size_t index) {
    RenderEncoding e;
    e.format = o.format;
    e.bitDepth = static_cast<int>(o.bitDepth.value_or(0));
    e.dither = o.dither.value_or(rs::Dither::Tpdf);  // ignored for float and lossy
    e.sampleRate = outputSampleRate(o, chainRate);
    if (o.mp3) {
        e.mp3Mode = o.mp3->mode.value_or(rs::Mp3Mode::Cbr);
        e.mp3Bitrate = static_cast<int>(o.mp3->bitrateKbps.value_or(320));
        e.mp3VbrQuality = static_cast<int>(o.mp3->vbrQuality.value_or(2));
    }
    e.vorbisQuality = o.vorbisQuality.value_or(6.0);
    e.ditherSeed = outputSeed(seed, index);
    return e;
}

// The text inserted before a later output's extension when it gives none
// and an earlier output has the same extension: ".16", ".128k", ".q6".
std::string defaultSuffix(const RenderEncoding& e) {
    if (e.bitDepth) return "." + std::to_string(e.bitDepth);
    if (e.format == rs::Format::Mp3)
        return e.mp3Mode == rs::Mp3Mode::Cbr ? "." + std::to_string(e.mp3Bitrate) + "k"
                                             : ".V" + std::to_string(e.mp3VbrQuality);
    return ".q" + compact(e.vorbisQuality);
}

Status insideRoot(const fs::path& root, const fs::path& path) {
    std::error_code ec;
    // The final path, not only its parent: an entry that already exists at
    // this name (a symlink pointing out of the renders directory) resolves
    // to its target here, so the check cannot be walked out of. The write
    // itself is a temp file plus an atomic rename (writeFileAtomic), which
    // replaces such an entry rather than following it.
    const auto resolved = fs::weakly_canonical(path, ec);
    if (ec)
        return Error("render.path", "cannot resolve the render path", "/pattern",
                     "use a relative pattern beneath a readable and writable renders "
                     "directory; remove traversal components or symlinks that escape it");
    const auto relative = resolved.lexically_relative(root);
    if (relative.empty() || *relative.begin() == ".." || relative.is_absolute())
        return Error("render.path", "render name escapes the renders directory", "/pattern",
                     "use a relative pattern beneath a readable and writable renders "
                     "directory; remove traversal components or symlinks that escape it");
    return okStatus();
}

// dryRun: nothing is written or rendered, so large inputs (the cover) are
// only checked.
Result<RenderPlan> plan(EngineApp& app, rs::Source operation, const Args& a, bool dryRun = false) {
    auto source = sessionFor(app, a.sel.branch, a.sel.version);
    if (!source) return std::move(source).error();
    model::Session s = *source;
    auto resolved = resolveFor(app, s, a);
    if (!resolved) return std::move(resolved).error();
    SUPPLE_TRY(checkSettings(resolved->effective, operation, resolved->from));
    RenderPlan out;
    out.operation = operation;
    out.settings = resolved->effective;
    out.branch = a.sel.branch.value_or(app.versions().branch());
    out.version = s.version;
    out.analyze = a.sel.analyze;
    const rs::Settings& e = out.settings;
    if (e.metadata && e.metadata->cover)
        SUPPLE_TRY(readCover(app, *e.metadata->cover, out, !dryRun));
    const auto& firstOutput = e.outputs->front();
    std::vector<RenderEncoding> encodings;
    for (std::size_t i = 0; i < e.outputs->size(); ++i)
        encodings.push_back(
            encodingOf((*e.outputs)[i], static_cast<int>(*e.sampleRate), s.project->seed, i));
    std::string pattern =
        a.sel.name.value_or(operation == rs::Source::Mix     ? *e.pattern->mix
                            : operation == rs::Source::Stems ? *e.pattern->stems
                                                             : *e.pattern->regions);
    // The mix's masters (ADR-0056): `master` for this call, else the
    // settings' masters, else the main master alone.
    std::vector<model::Id> mixMasters;
    if (operation == rs::Source::Mix) {
        if (a.sel.master) {
            auto id = trackId(s, *a.sel.master, "/master");
            if (!id) return std::move(id).error();
            mixMasters.push_back(*id);
        } else if (e.masters) {
            for (const auto& text : *e.masters) {
                auto id = trackId(s, text, "/masters");
                if (!id) return std::move(id).error();
                if (s.tracks.find(*id)->type != model::StripType::Master)
                    return Error("render.masters", "not a master: " + text, "/masters",
                                 "list master ids from track.list (type master)");
                if (std::find(mixMasters.begin(), mixMasters.end(), *id) == mixMasters.end())
                    mixMasters.push_back(*id);
            }
        } else {
            mixMasters.push_back(model::kMaster);
        }
        for (model::Id id : mixMasters)
            if (s.tracks.find(id)->type != model::StripType::Master)
                return Error("track.strip_type", "mix output must be a master", "/master",
                             "choose a master id from track.list");
        // Several masters: each file is told apart by its master's name.
        if (mixMasters.size() > 1 && pattern.find("{track") == std::string::npos)
            pattern += "-{track}";
    }
    std::error_code ec;
    const auto root = fs::weakly_canonical(app.rendersDir(), ec);
    if (ec)
        return Error("render.path", "cannot resolve renders directory", "/pattern",
                     "use a relative pattern beneath a readable and writable renders "
                     "directory; remove traversal components or symlinks that escape it");
    // The mix's range (and the stems'): bounds.
    double rangeFirst = 0, rangeLast = 0;
    switch (*e.bounds) {
        case rs::Bounds::Project:
            rangeLast = lastBeat(s);
            break;
        case rs::Bounds::Loop: {
            const auto& loop = app.loop();
            if (!(loop.end > loop.start))
                return Error("render.empty", "no loop is set", "/bounds",
                             "set a loop (transport.loop) or render the project or a custom range");
            rangeFirst = s.timeMap->beatAt(loop.start);
            rangeLast = s.timeMap->beatAt(loop.end);
            break;
        }
        case rs::Bounds::Custom:
            rangeFirst = e.startBeat.value_or(0);
            rangeLast = e.endBeat.value_or(lastBeat(s));
            break;
    }
    std::set<fs::path> destinations;
    auto add = [&](std::optional<model::Id> track, std::optional<model::Id> region, int tn,
                   int rn) -> Status {
        const auto* range = region ? s.regions.find(*region) : nullptr;
        const double first =
            range ? (range->timebase == model::Timebase::Time ? s.timeMap->beatAt(range->start)
                                                              : range->start)
                  : rangeFirst;
        const double last =
            range ? (range->timebase == model::Timebase::Time ? s.timeMap->beatAt(range->end)
                                                              : range->end)
                  : rangeLast;
        if (!(last > first))
            return Error("render.empty", "nothing to render", "/end_beat",
                         "add items or set an end beat");
        RenderTask task;
        task.session = s;
        task.pin = app.versions().pin(s.version);
        task.track = track;
        task.region = region;
        task.settings.startSeconds = s.timeMap->secondsAt(first);
        task.settings.endSeconds = s.timeMap->secondsAt(last);
        if (region && *e.handlesS > 0) {
            task.settings.startSeconds = std::max(0.0, task.settings.startSeconds - *e.handlesS);
            task.settings.endSeconds += *e.handlesS;
        }
        task.settings.projectDir = app.projectDir();
        task.settings.modules = app.modules();
        task.settings.tailSeconds = *e.tailS;
        SUPPLE_TRY(checkRenderRange(s, task.settings, *e.sampleRate));
        if (!track && a.sel.master) {
            auto id = trackId(s, *a.sel.master, "/master");
            if (!id) return std::move(id).error();
            if (s.tracks.find(*id)->type != model::StripType::Master)
                return Error("track.strip_type", "mix output must be a master", "/master",
                             "choose a master id from track.list");
            task.settings.master = *id;
        }
        const model::Track* strip = track ? s.tracks.find(*track) : nullptr;
        if (strip && strip->type == model::StripType::Master) {
            task.settings.master = *track;
        } else if (track) {
            if (*e.stems->mode == rs::StemMode::ThroughMaster)
                task.settings.throughMaster = track;
            else
                task.settings.track = track;
            task.settings.tap = *e.stems->tap == rs::StemTap::PreFx      ? model::Tap::PreFx
                                : *e.stems->tap == rs::StemTap::PreFader ? model::Tap::PreFader
                                                                         : model::Tap::PostFader;
        }
        task.channels =
            *e.channels == rs::kChannelsPerTrack
                ? (strip && strip->type != model::StripType::Master && strip->channels == 1 ? 1 : 2)
                : *e.channels;
        PatternData data;
        data.project = s.project->name.empty() ? "project" : s.project->name;
        data.track = strip ? strip->name : "";
        data.region = region ? s.regions.find(*region)->name : "";
        data.version = s.version;
        data.branch = out.branch;
        if (track && tn == 0) {
            for (const auto& entry : s.tracks) {
                if (entry.id == model::kMaster) continue;
                ++tn;
                if (entry.id == *track) break;
            }
        }
        data.trackNumber = tn;
        data.regionNumber = rn;
        data.startBeats = first;
        data.startSeconds = task.settings.startSeconds;
        if (track) {
            auto parent = strip->parent;
            std::vector<std::string> folders;
            while (parent && *parent != model::kMaster) {
                const auto* folder = s.tracks.find(*parent);
                if (!folder) break;
                folders.push_back(folder->name);
                parent = folder->parent;
            }
            for (auto it = folders.rbegin(); it != folders.rend(); ++it) {
                if (!data.folderPath.empty()) data.folderPath += '/';
                data.folderPath += *it;
            }
        }
        double nearestMarker = -1;
        for (const auto& marker : s.markers) {
            const double position = marker.value->timebase == model::Timebase::Time
                                        ? s.timeMap->beatAt(marker.value->pos)
                                        : marker.value->pos;
            if (position <= first && position >= nearestMarker) {
                nearestMarker = position;
                data.marker = marker.value->name;
            }
        }
        for (const auto& item : s.items)
            if ((!track || item.value->track == *track) &&
                model::itemSpan(s, *item.value).startBeat <= first &&
                model::itemEndBeat(s, *item.value) > first) {
                data.item = item.id.str();
                if (item.value->activeTake) {
                    const auto* take = s.takes.find(*item.value->activeTake);
                    if (take)
                        data.take = take->name.empty() ? item.value->activeTake->str() : take->name;
                }
                break;
            }
        data.sampleRate = encodings.front().sampleRate;
        data.bitDepth = encodings.front().bitDepth;
        data.format = std::string(rs::formatName(firstOutput.format));
        data.counter = static_cast<int>(out.tasks.size() + out.skipped.size() + 1);
        auto name = expand(pattern, data);
        if (!name) return std::move(name).error();
        task.label = *name;
        if (e.metadata) {
            const auto& m = *e.metadata;
            for (const auto& [field, from, to] :
                 {std::tuple{"title", &m.title, &task.tags.title},
                  std::tuple{"artist", &m.artist, &task.tags.artist},
                  std::tuple{"album", &m.album, &task.tags.album},
                  std::tuple{"genre", &m.genre, &task.tags.genre},
                  std::tuple{"comment", &m.comment, &task.tags.comment}}) {
                if (!*from) continue;
                auto text = expandText(**from, data);
                if (!text) {
                    auto error = std::move(text).error();
                    error.path = std::string("/metadata/") + field;
                    return error;
                }
                *to = std::move(*text);
            }
            task.tags.date = m.date.value_or("");
            task.tags.isrc = m.isrc.value_or("");
            if (m.trackNumber) task.tags.trackNumber = std::to_string(*m.trackNumber);
        }
        // Each output's path: the base name, a suffix and the extension.
        auto pathsFor = [&](const std::string& base) -> Result<std::vector<OutputFile>> {
            std::vector<OutputFile> files;
            std::set<std::string> extensions;
            for (std::size_t i = 0; i < encodings.size(); ++i) {
                const auto& o = (*e.outputs)[i];
                std::string suffix;
                if (i > 0)
                    suffix = o.suffix ? *o.suffix
                             : extensions.count(encodings[i].extension())
                                 ? defaultSuffix(encodings[i])
                                 : "";
                extensions.insert(encodings[i].extension());
                fs::path rel =
                    fs::path(base + suffix + encodings[i].extension()).lexically_normal();
                if (rel.is_absolute() || rel.empty() || *rel.begin() == "..")
                    return Error("render.path", "render name escapes directory", "/pattern",
                                 "use a relative pattern beneath a readable and writable renders "
                                 "directory; remove traversal components or symlinks that "
                                 "escape it");
                files.push_back({app.rendersDir() / rel, encodings[i]});
            }
            return files;
        };
        auto files = pathsFor(*name);
        if (!files) return std::move(files).error();
        auto anyExists = [](const std::vector<OutputFile>& list) {
            std::error_code err;
            return std::any_of(list.begin(), list.end(), [&](const OutputFile& f) {
                return fs::exists(fs::symlink_status(f.path, err));
            });
        };
        task.exists = anyExists(*files);
        if (task.exists) {
            switch (*e.ifExists) {
                case rs::IfExists::Overwrite:
                    break;
                case rs::IfExists::Error:
                    return Error("render.exists", files->front().path.string() + " exists",
                                 "/if_exists",
                                 "choose another file name, or if_exists overwrite, number or "
                                 "skip");
                case rs::IfExists::Skip: {
                    json::Value skip = json::Value::object();
                    if (track) skip["track"] = track->str();
                    if (region) skip["region"] = region->str();
                    skip["path"] = files->front().path.string();
                    skip["reason"] = "exists";
                    out.skipped.push_back(std::move(skip));
                    return okStatus();
                }
                case rs::IfExists::Number: {
                    bool found = false;
                    for (int n = 2; n < 10000 && !found; ++n) {
                        auto numbered = pathsFor(*name + "-" + std::to_string(n));
                        if (!numbered) return std::move(numbered).error();
                        bool taken = anyExists(*numbered);
                        for (const auto& f : *numbered)
                            taken |= destinations.count(f.path.lexically_normal()) > 0;
                        if (!taken) {
                            files = std::move(numbered);
                            task.label = *name + "-" + std::to_string(n);
                            found = true;
                        }
                    }
                    if (!found)
                        return Error("render.exists", "no free numbered name for " + *name,
                                     "/if_exists", "choose another file name");
                    break;
                }
            }
        }
        for (const auto& f : *files) {
            SUPPLE_TRY(insideRoot(root, f.path));
            // Atomic writes replace the final entry; only parent-directory
            // symlinks change their destination. Compare that parent
            // identity plus the final name, without following a final symlink.
            const auto parent = fs::weakly_canonical(f.path.parent_path(), ec);
            if (ec)
                return Error("render.path", "cannot resolve the render parent", "/pattern",
                             "choose a pattern under a readable renders directory");
            if (!destinations.insert(parent / f.path.filename()).second)
                return Error("render.path", "two renders have the same destination", "/pattern",
                             "include track, region or counter in a pattern that gives each file "
                             "a unique destination, or give later outputs a suffix");
            destinations.insert(f.path.lexically_normal());
        }
        task.outputs = std::move(*files);
        out.tasks.push_back(std::move(task));
        return {};
    };
    if (operation == rs::Source::Mix) {
        if (mixMasters == std::vector<model::Id>{model::kMaster}) {
            SUPPLE_TRY(add({}, {}, 0, 0));  // {track} stays empty, as before ADR-0056
        } else {
            int n = 0;
            for (model::Id id : mixMasters) SUPPLE_TRY(add(id, {}, ++n, 0));
        }
    } else if (operation == rs::Source::Stems) {
        std::vector<model::Id> ids;
        if (e.stems->tracks)
            for (const auto& text : *e.stems->tracks) {
                auto id = trackId(s, text, a.sel.tracks ? "/tracks" : "/stems/tracks");
                if (!id) return std::move(id).error();
                ids.push_back(*id);
            }
        else
            for (const auto& entry : s.tracks)
                if (entry.value->parent == model::kMaster) ids.push_back(entry.id);
        if (ids.empty())
            return Error(
                "render.empty", "no stem tracks", "/tracks",
                "add a channel or select existing track ids with tracks before rendering stems");
        int n = 0;
        for (auto id : ids) SUPPLE_TRY(add(id, {}, ++n, 0));
        if (*e.normalize->scope == rs::Scope::Master &&
            e.normalize->mode != rs::NormalizeMode::None && !out.tasks.empty()) {
            RenderTask master = out.tasks.front();
            master.settings.track.reset();
            master.settings.throughMaster.reset();
            master.settings.tap = model::Tap::PostFader;
            master.settings.master = model::kMaster;
            master.channels = *e.channels == 1 ? 1 : 2;
            master.outputs.clear();
            out.masterGain = std::move(master);
        }
    } else {
        std::vector<model::Id> regions;
        if (a.sel.regions)
            for (const auto& text : *a.sel.regions) {
                auto id = regionId(s, text);
                if (!id) return std::move(id).error();
                regions.push_back(*id);
            }
        else
            for (const auto& entry : s.regions) regions.push_back(entry.id);
        const rs::Matrix* matrix = e.matrix ? &*e.matrix : nullptr;
        const std::vector<std::string>* projectRow = nullptr;
        if (matrix)
            if (auto it = matrix->find("project"); it != matrix->end()) projectRow = &it->second;
        if (regions.empty() && !projectRow)
            return Error("render.empty", "no regions", "/regions",
                         "add a region with region.add or select existing ids from region.list");
        if (projectRow && !a.sel.regions)
            for (const auto& cell : *projectRow) {
                auto tr = trackId(s, cell, "/matrix");
                if (!tr) return std::move(tr).error();
                SUPPLE_TRY(add(*tr, {}, 0, 0));
            }
        int n = 0;
        for (auto id : regions) {
            ++n;
            const std::vector<std::string>* row = nullptr;
            if (matrix)
                if (auto it = matrix->find(id.str()); it != matrix->end()) row = &it->second;
            if (row) {
                for (const auto& cell : *row) {
                    auto tr = trackId(s, cell, "/matrix");
                    if (!tr) return std::move(tr).error();
                    SUPPLE_TRY(add(*tr, id, 0, n));
                }
            } else
                SUPPLE_TRY(add({}, id, 0, n));
        }
    }
    if (out.tasks.empty() && out.skipped.size() == 0)
        return Error("render.empty", "render selection produced no files", {},
                     "select at least one region or matrix cell");
    return out;
}

json::Value dryRunList(const RenderPlan& p) {
    json::Value list = json::Value::array();
    for (const auto& task : p.tasks) {
        json::Value entry = json::Value::object();
        entry["path"] = task.outputs.front().path.string();
        entry["seconds"] =
            *p.settings.seamlessLoop
                ? task.settings.endSeconds - task.settings.startSeconds
                : task.settings.endSeconds - task.settings.startSeconds + task.settings.tailSeconds;
        entry["exists"] = task.exists;
        json::Value outputs = json::Value::array();
        for (const auto& o : task.outputs) {
            json::Value file = json::Value::object();
            file["path"] = o.path.string();
            file["format"] = std::string(rs::formatName(o.encoding.format));
            if (o.encoding.bitDepth) file["bit_depth"] = o.encoding.bitDepth;
            file["sample_rate"] = o.encoding.sampleRate;
            outputs.push_back(std::move(file));
        }
        entry["outputs"] = std::move(outputs);
        list.push_back(std::move(entry));
    }
    for (const auto& skipped : p.skipped.asArray()) {
        json::Value entry = json::Value::object();
        entry["path"] = *skipped.find("path");
        entry["exists"] = true;
        entry["skip"] = true;
        list.push_back(std::move(entry));
    }
    return list;
}

// ------------------------------------------------------------------ running

struct Rendered {
    audio::AudioData audio;
    std::uint32_t outputLatency = 0;
    std::vector<std::string> warnings;
    std::map<std::string, json::Value> meters;
};

struct RunContext {
    explicit RunContext(EngineApp& a) : app(a) {}
    RunContext(EngineApp& a, cmd::JobManager* j, std::string id,
               const std::vector<host::PluginInfo>* c, const host::HostIdentity* i,
               supple::StopToken s)
        : app(a), jobs(j), job(std::move(id)), catalogue(c), identity(i), stop(std::move(s)) {}
    EngineApp& app;
    cmd::JobManager* jobs = nullptr;
    std::string job;
    const std::vector<host::PluginInfo>* catalogue = nullptr;
    const host::HostIdentity* identity = nullptr;
    supple::StopToken stop;
};

Error cancelledError() {
    return Error("job.cancelled", "render cancelled", {},
                 "submit a new render job if its output is still wanted; the cancelled job "
                 "cannot resume");
}

// Steps 1-4: render the range and tail, fold a seamless loop, resample to
// the chain's rate, fold to mono.
Result<Rendered> renderSteps(RunContext& run, const RenderPlan& p, const RenderTask& task,
                             std::size_t index, std::size_t count) {
    OfflineSettings settings = task.settings;
    if (run.jobs) {
        settings.cancelFlag = run.jobs->cancellationFlag(run.job);
        settings.progressFlag = run.jobs->progressFlag(run.job);
        settings.progress = [jobs = run.jobs, id = run.job](double fraction) {
            (void)jobs->progress(id, fraction, "Rendering");
        };
        settings.progressBase = static_cast<double>(index) / static_cast<double>(count);
        settings.progressScale = 0.8 / static_cast<double>(count);
    }
    settings.stopToken = run.stop;
    auto offline = run.catalogue && run.identity
                       ? renderSnapshot(task.session, settings, *run.catalogue, *run.identity)
                       : run.app.renderSession(task.session, settings);
    if (!offline) return std::move(offline).error();
    Rendered r;
    r.audio = std::move(offline->audio);
    r.outputLatency = offline->outputLatency;
    r.warnings = std::move(offline->warnings);
    r.meters = std::move(offline->meters);
    if (*p.settings.seamlessLoop) {
        const auto tail = time::samplesAt(task.settings.tailSeconds, r.audio.sampleRate);
        const auto frames = static_cast<std::int64_t>(r.audio.frames());
        if (frames > tail) foldSeamlessLoop(r.audio, static_cast<std::size_t>(frames - tail));
    }
    const double rate = static_cast<double>(*p.settings.sampleRate);
    if (rate != r.audio.sampleRate) {
        auto converted = audio::resample(r.audio, rate);
        if (!converted) return std::move(converted).error();
        r.audio = std::move(*converted);
    }
    if (task.channels == 1) foldMono(r.audio);
    return r;
}

double samplePeakDb(const audio::AudioData& data) {
    float peak = 0;
    for (const auto& channel : data.channels)
        for (float sample : channel) peak = std::max(peak, std::fabs(sample));
    return peak > 0 ? 20.0 * std::log10(static_cast<double>(peak)) : -1000.0;
}

Status writeOutput(const fs::path& path, std::span<const std::uint8_t> bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec)
        return Error("render.path", "cannot create " + path.parent_path().string(), "/pattern",
                     "check that the renders folder is writable");
    return writeFileAtomic(path, bytes);
}

// What one output carries besides audio (session-model §1.10 metadata).
// `startSeconds` is the file's first sample on the timeline.
Result<audio::FileMetadata> fileMetadata(const RenderPlan& p, const RenderTask& task,
                                         const RenderEncoding& encoding,
                                         const audio::AudioData& data, double startSeconds) {
    audio::FileMetadata m;
    m.tags = task.tags;
    m.tags.software = std::string(product::kName);
    m.tags.picture = p.cover;
    m.tags.pictureMime = p.coverMime;
    if (!p.settings.metadata) return m;
    const auto& settings = *p.settings.metadata;
    const model::Session& s = task.session;
    const double rate = static_cast<double>(encoding.sampleRate);
    const double playrate = s.project->playrate.rate;
    const double endSeconds = startSeconds + data.seconds() * playrate;
    auto frameAt = [&](double seconds) {
        return static_cast<std::uint64_t>(
            std::llround(std::max(0.0, seconds - startSeconds) / playrate * rate));
    };
    if (settings.markers.value_or(false) &&
        (encoding.format == rs::Format::Wav || encoding.format == rs::Format::Aiff)) {
        auto seconds = [&](double pos, model::Timebase timebase) {
            return timebase == model::Timebase::Time ? pos : s.timeMap->secondsAt(pos);
        };
        for (const auto& marker : s.markers) {
            const double at = seconds(marker.value->pos, marker.value->timebase);
            if (at >= startSeconds && at < endSeconds)
                m.cues.push_back({frameAt(at), 0, marker.value->name});
        }
        for (const auto& region : s.regions) {
            const double from = seconds(region.value->start, region.value->timebase);
            const double to = seconds(region.value->end, region.value->timebase);
            if (to <= startSeconds || from >= endSeconds) continue;
            const auto first = frameAt(std::max(from, startSeconds));
            const auto last = frameAt(std::min(to, endSeconds));
            m.cues.push_back({first, last > first ? last - first : 1, region.value->name});
        }
        std::stable_sort(m.cues.begin(), m.cues.end(),
                         [](const auto& a, const auto& b) { return a.frame < b.frame; });
    }
    if (settings.bwf.value_or(false) && encoding.format == rs::Format::Wav) {
        audio::Bext bext;
        bext.description = task.tags.title;
        bext.originator = std::string(product::kName);
        bext.originatorReference = task.label.substr(0, 32);
        if (settings.date && settings.date->size() == 10) bext.date = *settings.date;
        bext.timeReference =
            static_cast<std::uint64_t>(std::llround(std::max(0.0, startSeconds) * rate));
        auto measured = measureRender(data);
        if (!measured) return std::move(measured).error();
        bext.loudness = measured->integrated;
        bext.loudnessRange = measured->lra;
        bext.maxTruePeak = measured->truePeak;
        bext.maxMomentary = measured->maxMomentary;
        bext.maxShortTerm = measured->maxShortTerm;
        bext.codingHistory = "A=PCM,F=" + std::to_string(encoding.sampleRate) +
                             ",W=" + std::to_string(encoding.bitDepth) +
                             ",M=" + (data.channelCount() == 1 ? "mono" : "stereo") +
                             ",T=" + std::string(product::kName) + "\r\n";
        m.bext = std::move(bext);
    }
    return m;
}

// The whole chain for every task (engine.md §3). The result: {files, skipped}.
Result<json::Value> runTasks(RunContext& run, const RenderPlan& p,
                             std::map<std::string, json::Value>* mixMeters = nullptr) {
    const rs::Settings& e = p.settings;
    const auto& normalize = *e.normalize;
    json::Value files = json::Value::array();
    json::Value skipped = p.skipped;
    const std::size_t count = p.tasks.size() + (p.masterGain ? 1 : 0);
    std::optional<double> sharedGain;
    if (p.masterGain) {
        auto master = renderSteps(run, p, *p.masterGain, 0, count);
        if (!master) return std::move(master).error();
        auto measured = measureLevel(master->audio, normalize.mode);
        if (!measured) return std::move(measured).error();
        auto gain = normalizationGain(*measured, *normalize.target, *normalize.onlyReduce);
        if (!gain) return std::move(gain).error();
        sharedGain = *gain;
    }
    RenderCheckSpec checks;
    if (normalize.mode == rs::NormalizeMode::LufsI || normalize.mode == rs::NormalizeMode::LufsS ||
        normalize.mode == rs::NormalizeMode::LufsM)
        checks.loudnessMode = normalize.mode;
    checks.target = *normalize.target;
    checks.onlyReduce = *normalize.onlyReduce;
    checks.ceiling = e.limitTruePeakDb;
    for (std::size_t i = 0; i < p.tasks.size(); ++i) {
        if (run.jobs && run.jobs->cancelled(run.job)) return cancelledError();
        const auto& task = p.tasks[i];
        auto rendered = renderSteps(run, p, task, i + (p.masterGain ? 1 : 0), count);
        if (!rendered) return std::move(rendered).error();
        auto& data = rendered->audio;
        if (p.operation == rs::Source::Stems && *e.stems->skipSilent &&
            samplePeakDb(data) < -90.0) {
            json::Value skip = json::Value::object();
            if (task.track) skip["track"] = task.track->str();
            skip["path"] = task.outputs.front().path.string();
            skip["reason"] = "silent";
            skipped.push_back(std::move(skip));
            continue;
        }
        // 5. Normalization, 6. the true-peak limit, 7. trim.
        if (normalize.mode != rs::NormalizeMode::None) {
            for (const auto& channel : data.channels)
                for (float sample : channel)
                    if (!std::isfinite(sample))
                        return Error("render.normalize", "source audio contains non-finite samples",
                                     "/normalize",
                                     "bypass or replace the FX producing non-finite audio before "
                                     "normalizing");
            double gain = 1.0;
            if (sharedGain)
                gain = *sharedGain;
            else {
                auto measured = measureLevel(data, normalize.mode);
                if (!measured) return std::move(measured).error();
                auto g = normalizationGain(*measured, *normalize.target, *normalize.onlyReduce);
                if (!g) return std::move(g).error();
                gain = *g;
            }
            SUPPLE_TRY(applyGain(data, gain));
        }
        if (e.limitTruePeakDb) SUPPLE_TRY(limitTruePeak(data, *e.limitTruePeakDb));
        const std::size_t trimmed =
            trimSilence(data, *e.trim->start, *e.trim->end, *e.trim->thresholdDb);
        if (run.jobs && run.jobs->cancelled(run.job)) return cancelledError();
        const double startSeconds =
            task.settings.startSeconds +
            static_cast<double>(trimmed) / data.sampleRate * task.session.project->playrate.rate;
        json::Value entry = json::Value::object();
        json::Value outputs = json::Value::array();
        for (std::size_t k = 0; k < task.outputs.size(); ++k) {
            const auto& file = task.outputs[k];
            const audio::AudioData* source = &data;
            audio::AudioData converted;
            if (static_cast<double>(file.encoding.sampleRate) != data.sampleRate) {
                auto resampled = audio::resample(data, file.encoding.sampleRate);
                if (!resampled) return std::move(resampled).error();
                converted = std::move(*resampled);
                source = &converted;
            }
            const auto metadata = fileMetadata(p, task, file.encoding, *source, startSeconds);
            if (!metadata) return std::move(metadata).error();
            auto bytes = encodeRender(*source, file.encoding, *metadata);
            if (!bytes) return std::move(bytes).error();
            SUPPLE_TRY(writeOutput(file.path, *bytes));
            json::Value o = json::Value::object();
            o["path"] = file.path.string();
            o["format"] = std::string(rs::formatName(file.encoding.format));
            if (file.encoding.bitDepth) o["bit_depth"] = file.encoding.bitDepth;
            o["sample_rate"] = file.encoding.sampleRate;
            if (checks.loudnessMode || checks.ceiling || *e.report) {
                auto decoded = decodeRender(*bytes, file.encoding.format);
                if (!decoded) return std::move(decoded).error();
                auto measured = measureRender(*decoded);
                if (!measured) return std::move(measured).error();
                const json::Value result = renderChecks(*measured, checks);
                if (checks.loudnessMode || checks.ceiling) o["checks"] = result;
                if (*e.report) {
                    fs::path reportPath = file.path;
                    reportPath += ".loudness.txt";
                    SUPPLE_TRY(writeFileAtomic(
                        reportPath, loudnessReport(file.path.filename().string(), file.encoding,
                                                   *measured, result)));
                    o["report"] = reportPath.string();
                }
            }
            outputs.push_back(std::move(o));
        }
        entry["path"] = task.outputs.front().path.string();
        if (task.outputs.size() > 1) entry["secondary_path"] = task.outputs[1].path.string();
        entry["outputs"] = std::move(outputs);
        entry["branch"] = p.branch;
        entry["version"] = static_cast<std::int64_t>(p.version);
        entry["render"] = run.app.rememberRender(task.outputs.front().path);
        entry["start_beat"] = task.session.timeMap->beatAt(startSeconds);
        entry["seconds"] = data.seconds();
        entry["output_latency"] = static_cast<std::int64_t>(rendered->outputLatency);
        if (p.operation == rs::Source::Mix) entry["peak_dbfs"] = samplePeakDb(data);
        if (!rendered->warnings.empty()) {
            json::Value warnings = json::Value::array();
            for (const auto& warning : rendered->warnings) warnings.push_back(warning);
            entry["warnings"] = std::move(warnings);
        }
        if (p.operation != rs::Source::Mix || p.analyze) {
            auto report =
                analyzeAudio(data, p.analyze == RenderAnalyze::Full ? analysis::Detail::Full
                                                                    : analysis::Detail::Summary);
            if (report) entry["analysis"] = std::move(*report);
        }
        if (mixMeters && p.operation == rs::Source::Mix) *mixMeters = std::move(rendered->meters);
        files.push_back(std::move(entry));
        if (run.jobs)
            (void)run.jobs->progress(run.job, static_cast<double>(i + 1) / p.tasks.size(),
                                     "rendered " + task.label);
    }
    json::Value result = json::Value::object();
    result["files"] = std::move(files);
    if (skipped.size()) result["skipped"] = std::move(skipped);
    return result;
}

// render.mix answers with its one file's entry at the top level.
json::Value mixResult(json::Value result) {
    json::Value out = json::Value::object();
    const auto* files = result.find("files");
    if (files && files->size()) {
        out = files->at(0);
        if (const auto* seconds = out.find("seconds"))
            out["seconds"] = std::round(seconds->asDouble() * 1e4) / 1e4;
        if (const auto* peak = out.find("peak_dbfs"))
            out["peak_dbfs"] = std::round(peak->asDouble() * 100) / 100;
    }
    if (const auto* skipped = result.find("skipped")) out["skipped"] = *skipped;
    // Several masters (ADR-0056): the first file on top, every file listed.
    if (files && files->size() > 1) out["files"] = *files;
    return out;
}

rs::Source operationOf(std::string_view command, const Args& a, const rs::Settings& effective) {
    if (command == "render.mix") return rs::Source::Mix;
    if (command == "render.stems") return rs::Source::Stems;
    if (command == "render.regions") return rs::Source::Regions;
    if (a.sel.operation) return *a.sel.operation;
    if (a.sel.regions) return rs::Source::Regions;
    if (a.sel.tracks) return rs::Source::Stems;
    return effective.source.value_or(rs::Source::Mix);
}

Result<rs::Source> operationFor(EngineApp& app, std::string_view command, const Args& a) {
    if (command != "render.dry-run" || a.sel.operation || a.sel.regions || a.sel.tracks)
        return operationOf(command, a, {});
    auto source = sessionFor(app, a.sel.branch, a.sel.version);
    if (!source) return std::move(source).error();
    auto resolved = resolveFor(app, *source, a);
    if (!resolved) return std::move(resolved).error();
    return operationOf(command, a, resolved->effective);
}

json::Value filesForAdd(const json::Value& result) {
    const auto* files = result.find("files");
    return files ? *files : json::Value::array();
}

}  // namespace

Status checkRenderRange(const model::Session& s, const OfflineSettings& settings,
                        std::optional<std::int64_t> sampleRate) {
    SUPPLE_TRY(model::checkedSeconds(s, settings.startSeconds, "/start_beat"));
    SUPPLE_TRY(model::checkedSeconds(s, settings.endSeconds, "/end_beat"));
    if (!std::isfinite(settings.tailSeconds) || settings.tailSeconds < 0 ||
        settings.tailSeconds > 600)
        return model::numericError("/tail_s");
    SUPPLE_TRY(model::checkedSeconds(s, settings.endSeconds + settings.tailSeconds, "/tail_s"));
    const auto* rateDefault = s.project->render.find("sample_rate");
    const double rate = sampleRate    ? static_cast<double>(*sampleRate)
                        : rateDefault ? rateDefault->asDouble()
                                      : s.project->sampleRate;
    if (!std::isfinite(rate) || rate < 8000 || rate > 384000)
        return model::numericError("/sample_rate");
    SUPPLE_TRY(model::checkedSeconds(s, settings.endSeconds + settings.tailSeconds, "/end_beat",
                                     std::max(rate, s.project->sampleRate)));
    double minimumRate = 1;
    if (settings.applyMasterPlayrate) {
        minimumRate = s.project->playrate.rate;
        if (const auto env = model::activePlayrateEnvelope(s)) {
            const auto points = model::effectivePoints(s, *env);
            minimumRate = model::kMaxPlayrate;
            for (const auto& point : points) minimumRate = std::min(minimumRate, point.value);
        }
    }
    const long double duration = (static_cast<long double>(settings.endSeconds) -
                                  static_cast<long double>(settings.startSeconds)) /
                                     static_cast<long double>(minimumRate) +
                                 static_cast<long double>(settings.tailSeconds);
    if (!(settings.endSeconds > settings.startSeconds) || !(duration > 0))
        return Error("render.empty", "the end must come after the start", "/end_beat",
                     "set end_beat later than start_beat");
    // Match the engine's existing in-memory output/variant capacity, including resampling.
    const long double frames =
        std::ceil(duration * static_cast<long double>(std::max(rate, s.project->sampleRate)));
    if (!std::isfinite(frames) || frames > (std::int64_t{1} << 28) - 8192)
        return Error("render.budget", "render exceeds the in-memory frame budget", "/end_beat",
                     "render a shorter range, increase master playrate or use a lower sample rate");
    return {};
}

Result<OfflineResult> renderSnapshot(const model::Session& session, OfflineSettings settings,
                                     const std::vector<host::PluginInfo>& catalogue,
                                     const host::HostIdentity& identity) {
    SUPPLE_TRY(checkRenderRange(session, settings));
    InstancePool pool(
        [&catalogue](const model::PluginRef& ref) -> Result<host::PluginInfo> {
            for (const auto& plugin : catalogue)
                if (plugin.id == ref.id) return plugin;
            return Error(
                "plugin.unknown", "plugin unavailable in the render snapshot: " + ref.id, {},
                "select an installed plugin from plugin.list before queuing the render snapshot");
        },
        identity);
    return renderOffline(session, pool, settings);
}

Result<std::string> expandRenderPattern(std::string_view pattern,
                                        const RenderPatternContext& context) {
    return expand(pattern, context);
}

RenderTarget renderTarget(const EngineApp& app) {
    return {app.projectGeneration(), app.versions().branch()};
}
Status checkRenderTarget(const EngineApp& app, const RenderTarget& target) {
    if (app.projectGeneration() != target.generation || app.versions().branch() != target.branch)
        return Error("render.stale_target", "the render target project or branch changed", {},
                     "files remain on disk; import them into the intended project manually");
    return {};
}
void completeRenderJob(EngineApp& app, cmd::Dispatcher& dispatcher, cmd::JobManager& jobs,
                       std::string id, RenderTarget target, json::Value result,
                       std::function<void()> publish) {
    dispatcher.post("render.complete",
                    [&app, &jobs, id = std::move(id), target = std::move(target),
                     result = std::move(result), publish = std::move(publish)]() mutable -> Status {
                        if (auto valid = checkRenderTarget(app, target); !valid) {
                            json::Value error = json::Value::object();
                            error["code"] = valid.error().code;
                            error["message"] = valid.error().message;
                            error["path"] = valid.error().path;
                            error["hint"] = valid.error().hint;
                            result["status"] = "render.stale_target";
                            result["error"] = std::move(error);
                            app.emit("render.stale_target", result);
                            // Project replacement may already have cancelled the job.
                            if (jobs.cancelled(id)) return jobs.cancel(id, std::move(result));
                            return jobs.complete(id, std::move(result));
                        }
                        if (jobs.cancelled(id)) return {};
                        return jobs.completeWith(id, std::move(result), publish);
                    });
}

void postAddToProject(EngineApp& app, RenderTarget target, cmd::Dispatcher& dispatcher,
                      const cmd::Origin& origin, const json::Value& files) {
    std::vector<std::pair<std::string, double>> add;  // path, start beat
    if (files.isArray())
        for (const json::Value& f : files.asArray())
            if (const auto* path = f.find("path"); path && path->isString()) {
                const auto* start = f.find("start_beat");
                add.emplace_back(path->asString(),
                                 start && start->isNumber() ? start->asDouble() : 0.0);
            }
    if (add.empty()) return;
    dispatcher.post(
        "render.add_to_project",
        [&app, target = std::move(target), &dispatcher, origin, add]() -> Status {
            SUPPLE_TRY(checkRenderTarget(app, target));
            auto done =
                dispatcher.transact("Add renders to project", origin, [&]() -> Result<json::Value> {
                    for (const auto& [path, start] : add) {
                        json::Value track = json::Value::object();
                        track["name"] = fs::path(path).stem().string();
                        auto t = dispatcher.execute({"track.add", std::move(track), origin});
                        if (!t) return t;
                        json::Value item = json::Value::object();
                        item["track"] = (*t)["track"];
                        item["position"] = start;
                        item["audio_file"] = path;
                        if (auto i = dispatcher.execute({"item.add", std::move(item), origin}); !i)
                            return i;
                    }
                    return json::Value::object();
                });
            if (!done) return std::move(done).error();
            return {};
        },
        cmd::Dispatcher::PostKind::Edit);
}

namespace {

struct QueueAddArgs {
    std::string command;
    json::Value args = json::Value::object();
    std::optional<std::string> label;
};
struct QueueRemoveArgs {
    std::optional<std::string> id;
    bool all = false;
};
struct QueueRunArgs {
    bool job = false;
};

json::Value queueEntry(const EngineApp::QueuedRender& q) {
    json::Value e = json::Value::object();
    e["id"] = q.id;
    e["command"] = q.command;
    e["label"] = q.label;
    e["version"] = static_cast<std::int64_t>(q.version);
    e["files"] = q.files;
    e["args"] = q.args;
    return e;
}

// One queued render ready to run: its stored arguments decoded and planned.
struct QueuedWork {
    std::string id, label;
    RenderPlan plan;
};

Result<QueuedWork> planQueued(EngineApp& app, const EngineApp::QueuedRender& q) {
    auto decoded = argsSchema().decode(q.args);
    if (!decoded) return std::move(decoded).error();
    decoded->sel.job = false;
    auto planned = plan(app, operationOf(q.command, *decoded, {}), *decoded);
    if (!planned) return std::move(planned).error();
    return QueuedWork{q.id, q.label, std::move(*planned)};
}

Error queueArgsError(Error error) {
    auto prefix = [](auto&& self, Error& e) -> void {
        e.path = "/args/args" + e.path;
        for (auto& detail : e.details) self(self, detail);
    };
    prefix(prefix, error);
    return error;
}

Status queueDone(EngineApp& app, const RenderTarget& target, const std::vector<std::string>& done) {
    SUPPLE_TRY(checkRenderTarget(app, target));
    std::erase_if(app.renderQueue(), [&](const auto& q) {
        return std::find(done.begin(), done.end(), q.id) != done.end();
    });
    return {};
}

void registerQueueCommands(cmd::Registry& registry) {
    {
        Command<QueueAddArgs, json::Value> c;
        c.name = "render.queue.add";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Queue a render (mix, stems or regions) to run later with render.queue.run.";
        c.description =
            "args: the render command's arguments (as for render.mix/stems/regions). The queued "
            "render is pinned to the session and settings as they are now (its version, a "
            "branch's head when branch is given, and the effective render settings, a loop "
            "range as beats), so later edits do not change it; a dry run lists its files now. "
            "The queue is transport-like state: not an undo step, not saved with the project.";
        c.tags = {"render"};
        c.args = schema::object<QueueAddArgs>("render_queue_add_args")
                     .field("command", &QueueAddArgs::command,
                            schema::string().pattern("^render\\.(mix|stems|regions)$",
                                                     "render.mix, render.stems or render.regions"))
                     .field("args", &QueueAddArgs::args, schema::any())
                     .field("label", &QueueAddArgs::label, schema::string())
                     .build();
        c.result = schema::any();
        c.errors = {{"render.empty", "the render would write nothing", "add items or a range"}};
        c.examples = {{"Queue the mix as it is now",
                       obj(R"({"command":"render.mix","args":{"end_beat":1,"name":"queued"}})"),
                       obj(R"({"id":"q1","command":"render.mix","label":"<any>","version":"<any>",
                               "files":"<any>","args":"<any>"})"),
                       "song"}};
        c.run = [](const QueueAddArgs& a, Context& ctx) -> Result<json::Value> {
            auto& app = ctx.service<EngineApp>();
            auto decoded = argsSchema().decode(a.args);
            if (!decoded) return queueArgsError(std::move(decoded).error());
            Args args = std::move(*decoded);
            args.sel.job = false;
            if (!args.sel.version) {  // pin it: later edits must not change what renders
                auto source = sessionFor(app, args.sel.branch);
                if (!source) return queueArgsError(std::move(source).error());
                args.sel.version = static_cast<std::int64_t>(source->version);
            }
            const auto operation = operationOf(a.command, args, {});
            auto planned = plan(app, operation, args, true);  // run plans again, reading all
            if (!planned) return queueArgsError(std::move(planned).error());
            // The settings as they resolve now, so a later default, preset or
            // loop does not change the queued render.
            Args pinned;
            pinned.sel.version = args.sel.version;
            pinned.sel.master = args.sel.master;
            pinned.sel.name = args.sel.name;
            pinned.sel.regions = args.sel.regions;
            pinned.sel.analyze = args.sel.analyze;
            pinned.settings = planned->settings;
            if (pinned.settings.bounds == rs::Bounds::Loop) {
                const auto& session =
                    planned->tasks.empty() ? app.view() : planned->tasks[0].session;
                pinned.settings.bounds = rs::Bounds::Custom;
                pinned.settings.startBeat = session.timeMap->beatAt(app.loop().start);
                pinned.settings.endBeat = session.timeMap->beatAt(app.loop().end);
            }
            EngineApp::QueuedRender q;
            q.id = app.nextQueueId();
            q.command = a.command;
            q.version = static_cast<std::uint64_t>(*args.sel.version);
            q.pin = app.versions().pin(q.version);
            q.args = argsSchema().encode(pinned);
            q.files = json::Value::array();
            for (const auto& task : planned->tasks)
                q.files.push_back(task.outputs.front().path.string());
            const std::string what = a.command.substr(std::string_view("render.").size());
            const auto n = planned->tasks.size();
            q.label = a.label.value_or(what + " @ v" + std::to_string(q.version) + " (" +
                                       std::to_string(n) + (n == 1 ? " file)" : " files)"));
            json::Value out = queueEntry(q);
            app.renderQueue().push_back(std::move(q));
            return out;
        };
        registry.add(std::move(c));
    }
    {
        Command<cmd::NoArgs, json::Value> c;
        c.name = "render.queue.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "The queued renders, in the order render.queue.run runs them.";
        c.tags = {"render"};
        c.args = schema::object<cmd::NoArgs>("no_args").build();
        c.result = schema::any();
        c.examples = {{"Empty", obj("{}"), obj(R"({"queue":[]})"), "empty"}};
        c.run = [](const cmd::NoArgs&, Context& ctx) -> Result<json::Value> {
            json::Value list = json::Value::array();
            for (const auto& q : ctx.service<EngineApp>().renderQueue())
                list.push_back(queueEntry(q));
            json::Value out = json::Value::object();
            out["queue"] = std::move(list);
            return out;
        };
        registry.add(std::move(c));
    }
    {
        Command<QueueRemoveArgs, json::Value> c;
        c.name = "render.queue.remove";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Take a render off the queue (id), or clear it (all: true).";
        c.tags = {"render"};
        c.args = schema::object<QueueRemoveArgs>("render_queue_remove_args")
                     .field("id", &QueueRemoveArgs::id, schema::string())
                     .field("all", &QueueRemoveArgs::all, schema::boolean())
                     .defaultValue(false)
                     .build();
        c.result = schema::any();
        c.errors = {{"render.queue_unknown", "no queued render with that id",
                     "render.queue.list shows them"}};
        c.examples = {{"Clear", obj(R"({"all":true})"), obj(R"({"removed":0})"), "empty"}};
        c.run = [](const QueueRemoveArgs& a, Context& ctx) -> Result<json::Value> {
            auto& queue = ctx.service<EngineApp>().renderQueue();
            json::Value out = json::Value::object();
            if (a.all) {
                out["removed"] = static_cast<std::int64_t>(queue.size());
                queue.clear();
                return out;
            }
            if (!a.id)
                return Error("render.queue_unknown", "give id or all", "/id",
                             "render.queue.list shows the ids");
            const auto before = queue.size();
            std::erase_if(queue, [&](const auto& q) { return q.id == *a.id; });
            if (queue.size() == before)
                return Error("render.queue_unknown", "no queued render " + *a.id, "/id",
                             "render.queue.list shows the ids");
            out["removed"] = 1;
            return out;
        };
        registry.add(std::move(c));
    }
    {
        Command<QueueRunArgs, json::Value> c;
        c.name = "render.queue.run";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Run every queued render in order; each one leaves the queue when it is done.";
        c.description =
            "Each render renders the version it was queued at. job: true runs them as one job "
            "(job.get for progress). The result lists each render's files under its id.";
        c.tags = {"render"};
        c.args = schema::object<QueueRunArgs>("render_queue_run_args")
                     .field("job", &QueueRunArgs::job, schema::boolean())
                     .defaultValue(false)
                     .build();
        c.result = schema::any();
        c.errors = {{"render.queue_empty", "nothing is queued", "render.queue.add first"}};
        c.examples = {
            {"Render the queue", obj("{}"), obj(R"({"renders":"<any>"})"), "render_queued"}};
        c.run = [](const QueueRunArgs& a, Context& ctx) -> Result<json::Value> {
            auto& app = ctx.service<EngineApp>();
            if (app.renderQueue().empty())
                return Error("render.queue_empty", "the render queue is empty", {},
                             "render.queue.add a render first");
            std::vector<QueuedWork> work;
            for (const auto& q : app.renderQueue()) {  // all planned before any renders
                auto w = planQueued(app, q);
                if (!w) return std::move(w).error();
                work.push_back(std::move(*w));
            }
            auto& dispatcher = ctx.service<cmd::Dispatcher>();
            const cmd::Origin origin = ctx.origin();
            const auto target = renderTarget(app);
            auto entryOf = [](const QueuedWork& w, const json::Value& result) {
                json::Value entry = json::Value::object();
                entry["id"] = w.id;
                entry["label"] = w.label;
                entry["files"] = filesForAdd(result);
                if (const auto* skipped = result.find("skipped")) entry["skipped"] = *skipped;
                return entry;
            };
            if (!a.job) {
                json::Value renders = json::Value::array();
                for (auto& w : work) {
                    RunContext run{app};
                    auto result = runTasks(run, w.plan);
                    if (!result) return std::move(result).error();
                    std::erase_if(app.renderQueue(), [&](const auto& q) { return q.id == w.id; });
                    if (*w.plan.settings.addToProject)
                        postAddToProject(app, target, dispatcher, origin, filesForAdd(*result));
                    renders.push_back(entryOf(w, *result));
                }
                json::Value out = json::Value::object();
                out["renders"] = std::move(renders);
                return out;
            }
            auto available = app.plugins();
            if (!available) return std::move(available).error();
            std::vector<host::PluginInfo> catalogue(available->begin(), available->end());
            const auto identity = app.options().identity;
            auto& jobs = ctx.service<cmd::JobManager>();
            const std::string id = jobs.create();
            app.startJob([&app, &jobs, &dispatcher, origin, target, id, work = std::move(work),
                          catalogue = std::move(catalogue), identity,
                          entryOf](supple::StopToken stop) {
                json::Value renders = json::Value::array();
                std::vector<std::string> done;
                for (const auto& w : work) {
                    if (stop.stop_requested() || jobs.cancelled(id)) return;
                    RunContext run{app, &jobs, id, &catalogue, &identity, stop};
                    auto result = runTasks(run, w.plan);
                    if (!result) {
                        if (!jobs.cancelled(id)) (void)jobs.fail(id, std::move(result).error());
                        // What finished leaves the queue (on the model thread).
                        dispatcher.post("render.queue.done", [&app, target, done]() -> Status {
                            return queueDone(app, target, done);
                        });
                        return;
                    }
                    renders.push_back(entryOf(w, *result));
                    done.push_back(w.id);
                }
                json::Value out = json::Value::object();
                const auto completedRenders = renders;
                out["renders"] = std::move(renders);
                completeRenderJob(
                    app, dispatcher, jobs, id, target, std::move(out),
                    [&app, &dispatcher, target, done, origin, work, completedRenders] {
                        for (std::size_t index = 0; index < work.size(); ++index)
                            if (*work[index].plan.settings.addToProject)
                                postAddToProject(app, target, dispatcher, origin,
                                                 *completedRenders.at(index).find("files"));
                        dispatcher.post("render.queue.done", [&app, target, done]() -> Status {
                            return queueDone(app, target, done);
                        });
                    });
            });
            json::Value out = json::Value::object();
            out["job"] = id;
            out["status"] = "running";
            return out;
        };
        registry.add(std::move(c));
    }
}

struct PresetSaveArgs {
    std::optional<std::string> name;
    bool isDefault = false;
    std::optional<rs::Settings> settings;
};
struct PresetNameArgs {
    std::optional<std::string> name;
    bool isDefault = false;
};

Status presetTarget(const std::optional<std::string>& name, bool isDefault) {
    if (name.has_value() == isDefault)
        return Error("render.preset_name", "give name or default: true", "/name",
                     "name a preset, or pass default: true for your render default");
    return okStatus();
}

json::Value presetJson(const RenderPreset& preset) {
    json::Value out = json::Value::object();
    out["name"] = preset.name;
    out["factory"] = preset.factory;
    out["settings"] = rs::toJson(preset.settings);
    return out;
}

void registerSettingsCommands(cmd::Registry& registry) {
    {
        Command<Args, json::Value> c;
        c.name = "render.settings";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "The render settings that apply, and the layer each one comes from.";
        c.description =
            "effective: what a render with these arguments would use (every member filled in); "
            "project: project.render as stored; default: the user's render default (null if "
            "none); from: per member, args, project, default or built-in (ADR-0055). Change the "
            "project's settings with project.set {render} or render.preset.load.";
        c.tags = {"render"};
        c.args = argsSchema();
        c.result = schema::any();
        c.examples = {{"What File → Mix would do", obj("{}"),
                       obj(R"({"effective":"<any>","project":"<any>","default":null,
                               "from":"<any>"})"),
                       "song"}};
        c.run = [](const Args& a, Context& ctx) -> Result<json::Value> {
            auto& app = ctx.service<EngineApp>();
            auto source = sessionFor(app, a.sel.branch, a.sel.version);
            if (!source) return std::move(source).error();
            auto resolved = resolveFor(app, *source, a);
            if (!resolved) return std::move(resolved).error();
            json::Value out = json::Value::object();
            out["effective"] = rs::toJson(resolved->effective);
            out["project"] = rs::toJson(projectRenderSettings(*source->project));
            out["default"] =
                resolved->userDefault ? rs::toJson(*resolved->userDefault) : json::Value();
            out["from"] = originsJson(resolved->from);
            return out;
        };
        registry.add(std::move(c));
    }
    {
        Command<cmd::NoArgs, json::Value> c;
        c.name = "render.preset.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Render presets: the factory ones, then the user's, with their settings.";
        c.description =
            "default: the user's render default, or null. Presets live in the user's data "
            "folder (render-presets/), shared by every project.";
        c.tags = {"render"};
        c.args = schema::object<cmd::NoArgs>("no_args").build();
        c.result = schema::any();
        c.examples = {{"List", obj("{}"), obj(R"({"presets":"<any>","default":null})"), "empty"}};
        c.run = [](const cmd::NoArgs&, Context& ctx) -> Result<json::Value> {
            auto& app = ctx.service<EngineApp>();
            auto presets = listRenderPresets(app);
            if (!presets) return std::move(presets).error();
            json::Value list = json::Value::array();
            for (const auto& preset : *presets) list.push_back(presetJson(preset));
            auto user = loadRenderDefault(app);
            if (!user) return std::move(user).error();
            json::Value out = json::Value::object();
            out["presets"] = std::move(list);
            out["default"] = *user ? rs::toJson(**user) : json::Value();
            return out;
        };
        registry.add(std::move(c));
    }
    auto nameField = [] { return schema::string().doc("A preset name (render.preset.list)"); };
    {
        Command<PresetSaveArgs, json::Value> c;
        c.name = "render.preset.save";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Save render settings as a named preset, or as the user's render default.";
        c.description =
            "settings: what to save (default: the effective settings now). Never saved: "
            "stems.tracks, matrix and custom bounds, which belong to one project. A file per "
            "preset in the user's data folder; factory presets cannot be overwritten.";
        c.tags = {"render"};
        c.args = schema::object<PresetSaveArgs>("render_preset_save_args")
                     .field("name", &PresetSaveArgs::name, nameField())
                     .field("default", &PresetSaveArgs::isDefault, schema::boolean())
                     .defaultValue(false)
                     .field("settings", &PresetSaveArgs::settings, rs::settingsSchema())
                     .build();
        c.result = schema::any();
        c.errors = {
            {"render.preset_factory", "a factory preset has that name", "save under another name"}};
        c.examples = {{"A preset of FLAC files",
                       obj(R"({"name":"Archive","settings":{"outputs":[{"format":"flac"}]}})"),
                       obj(R"({"name":"Archive"})"), "empty"}};
        c.run = [](const PresetSaveArgs& a, Context& ctx) -> Result<json::Value> {
            auto& app = ctx.service<EngineApp>();
            SUPPLE_TRY(presetTarget(a.name, a.isDefault));
            rs::Settings settings;
            if (a.settings)
                settings = *a.settings;
            else {
                auto resolved = resolveFor(app, app.view(), Args{});
                if (!resolved) return std::move(resolved).error();
                settings = resolved->effective;
            }
            SUPPLE_TRY(saveRenderPreset(app, a.name, settings));
            json::Value out = json::Value::object();
            if (a.name)
                out["name"] = *a.name;
            else
                out["default"] = true;
            return out;
        };
        registry.add(std::move(c));
    }
    {
        Command<PresetNameArgs, json::Value> c;
        c.name = "render.preset.load";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Replace the project's render settings with a preset (one undo step).";
        c.tags = {"render"};
        c.args = schema::object<PresetNameArgs>("render_preset_load_args")
                     .field("name", &PresetNameArgs::name, nameField())
                     .field("default", &PresetNameArgs::isDefault, schema::boolean())
                     .defaultValue(false)
                     .build();
        c.result = schema::any();
        c.errors = {
            {"render.preset_unknown", "no preset with that name", "render.preset.list shows them"}};
        c.examples = {{"A factory preset", obj(R"({"name":"WAV 24-bit"})"),
                       obj(R"({"render":"<any>"})"), "empty"}};
        c.run = [](const PresetNameArgs& a, Context& ctx) -> Result<json::Value> {
            auto& app = ctx.service<EngineApp>();
            SUPPLE_TRY(presetTarget(a.name, a.isDefault));
            rs::Settings settings;
            if (a.name) {
                auto found = findRenderPreset(app, *a.name);
                if (!found) return std::move(found).error();
                settings = std::move(*found);
            } else {
                auto user = loadRenderDefault(app);
                if (!user) return std::move(user).error();
                if (!*user)
                    return Error("render.preset_unknown", "no user default saved", "/default",
                                 "render.preset.save {default: true} first");
                settings = std::move(**user);
            }
            model::EditTxn& t = app.txn();
            model::Project p = t.project();
            p.render = rs::toJson(settings);
            t.setProject(std::move(p));
            json::Value out = json::Value::object();
            out["render"] = rs::toJson(settings);
            return out;
        };
        registry.add(std::move(c));
    }
    {
        Command<PresetNameArgs, json::Value> c;
        c.name = "render.preset.delete";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Delete one of the user's render presets, or the user's render default.";
        c.tags = {"render"};
        c.args = schema::object<PresetNameArgs>("render_preset_delete_args")
                     .field("name", &PresetNameArgs::name, nameField())
                     .field("default", &PresetNameArgs::isDefault, schema::boolean())
                     .defaultValue(false)
                     .build();
        c.result = schema::any();
        c.errors = {{"render.preset_factory", "factory presets cannot be deleted",
                     "delete one of your own"}};
        c.examples = {{"Delete a preset", obj(R"({"name":"Archive"})"), obj(R"({"deleted":true})"),
                       "render_preset"}};
        c.run = [](const PresetNameArgs& a, Context& ctx) -> Result<json::Value> {
            auto& app = ctx.service<EngineApp>();
            SUPPLE_TRY(presetTarget(a.name, a.isDefault));
            SUPPLE_TRY(deleteRenderPreset(app, a.name));
            json::Value out = json::Value::object();
            out["deleted"] = true;
            return out;
        };
        registry.add(std::move(c));
    }
}

}  // namespace

void registerRenderCommands(cmd::Registry& registry) {
    for (const char* name : {"render.mix", "render.stems", "render.regions", "render.dry-run"}) {
        Command<Args, json::Value> c;
        c.name = name;
        const std::string_view which(name);
        c.kind = which == "render.dry-run" ? Kind::Query : Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary =
            which == "render.mix"
                ? "Render the mix offline with the project's render settings (default: whole song)."
            : which == "render.stems" ? "Render each selected track to its own file (stems)."
            : which == "render.regions"
                ? "Render each region (or the render matrix's tracks per region) to files."
                : "List the files a render would write, with lengths, without rendering.";
        c.description =
            "Arguments override project.render (then the user's default, then built-ins) for "
            "this call only; render.settings shows what applies. Files go to the renders folder, "
            "named by pattern (subfolders allowed). job: true runs the render as a job (job.get "
            "for progress, job.cancel to stop). Each file lists its outputs (with delivery "
            "checks when a loudness target or a ceiling is set), branch, version, start_beat, "
            "seconds, a render id (analyze.render, analyze.compare) and an analysis summary.";
        if (which == "render.mix")
            c.description +=
                " The mix answers with its file's entry at the top level (path, secondary_path, "
                "outputs, peak_dbfs, …); analyze: summary or full adds the report inline. "
                "masters renders several masters, a file each (the first on top, all in files); "
                "a pattern without {track} then gets -{track}, the master's name.";
        if (which == "render.dry-run")
            c.description =
                "Each file: path (where if_exists sends it), exists (its destination is already "
                "there), skip (if_exists skip leaves it out), seconds and outputs. operation: "
                "mix, stems or regions (default: tracks or regions given, else the settings' "
                "source).";
        c.tags = {"render"};
        c.args = argsSchema();
        c.result = schema::any();
        if (which == "render.mix")
            c.examples = {
                {"Render", obj(R"({"name": "mix-v1"})"),
                 obj(R"({"render": "rd_1", "path": "<any>", "outputs": "<any>", "branch": "main",
                         "version": "<any>", "start_beat": 0, "seconds": "<any>",
                         "peak_dbfs": "<any>", "output_latency": 0})"),
                 "song_with_fx"}};
        else
            c.examples = {{"Render project",
                           obj(R"({"end_beat":1,"pattern":"example-{counter:02}"})"),
                           obj(R"({"files":"<any>"})"),
                           which == "render.regions" ? "song_with_region" : "song"}};
        c.run = [name](const Args& a, Context& ctx) -> Result<json::Value> {
            auto& app = ctx.service<EngineApp>();
            auto operation = operationFor(app, name, a);
            if (!operation) return std::move(operation).error();
            auto planned = plan(app, *operation, a, std::string_view(name) == "render.dry-run");
            if (!planned) return std::move(planned).error();
            const bool mix = std::string_view(name) == "render.mix";
            if (std::string_view(name) == "render.dry-run") {
                json::Value out = json::Value::object();
                out["files"] = dryRunList(*planned);
                return out;
            }
            auto& dispatcher = ctx.service<cmd::Dispatcher>();
            const cmd::Origin origin = ctx.origin();
            const auto target = renderTarget(app);
            const bool addToProject = *planned->settings.addToProject;
            if (!a.sel.job) {
                RunContext run{app};
                std::map<std::string, json::Value> meters;
                auto result = runTasks(run, *planned, &meters);
                if (!result) return std::move(result).error();
                if (mix) app.rememberRenderMeters(std::move(meters));
                if (addToProject)
                    postAddToProject(app, target, dispatcher, origin, filesForAdd(*result));
                return mix ? mixResult(std::move(*result)) : std::move(*result);
            }
            auto available = app.plugins();
            if (!available) return std::move(available).error();
            std::vector<host::PluginInfo> catalogue(available->begin(), available->end());
            const auto identity = app.options().identity;
            auto& jobs = ctx.service<cmd::JobManager>();
            const std::string id = jobs.create();
            app.startJob([&app, &jobs, &dispatcher, origin, target, id,
                          planned = std::move(*planned), mix, addToProject,
                          catalogue = std::move(catalogue), identity](supple::StopToken stop) {
                if (stop.stop_requested() || jobs.cancelled(id)) return;
                RunContext run{app, &jobs, id, &catalogue, &identity, stop};
                auto meters = std::make_shared<std::map<std::string, json::Value>>();
                auto result = runTasks(run, planned, meters.get());
                if (result) {
                    json::Value files = filesForAdd(*result);
                    completeRenderJob(
                        app, dispatcher, jobs, id, target,
                        mix ? mixResult(std::move(*result)) : std::move(*result),
                        [&app, target, &dispatcher, origin, files, addToProject, mix, meters] {
                            if (mix) app.rememberRenderMeters(std::move(*meters));
                            if (addToProject)
                                postAddToProject(app, target, dispatcher, origin, files);
                        });
                } else if (!jobs.cancelled(id))
                    (void)jobs.fail(id, std::move(result).error());
            });
            json::Value out = json::Value::object();
            out["job"] = id;
            out["status"] = "running";
            return out;
        };
        registry.add(std::move(c));
    }
    Command<AbArgs, json::Value> ab;
    ab.name = "branch.render-ab";
    ab.kind = Kind::Action;
    ab.exposure = cmd::Exposure::McpTool;
    ab.summary = "Render two branch heads over the same range and compare them.";
    ab.tags = {"branch", "render"};
    ab.args = schema::object<AbArgs>("render_ab_args")
                  .field("a", &AbArgs::a, schema::string())
                  .field("b", &AbArgs::b, schema::string())
                  .field("start_beat", &AbArgs::startBeat, schema::number().min(0))
                  .field("end_beat", &AbArgs::endBeat, schema::number().exclusiveMin(0))
                  .field("tail_s", &AbArgs::tailS, schema::number().range(0, 600))
                  .build();
    ab.result = schema::any();
    ab.examples = {{"Compare main with itself", obj(R"({"a":"main","b":"main","end_beat":1})"),
                    obj(R"({"a":"<any>","b":"<any>","comparison":"<any>"})"), "song"}};
    ab.run = [](const AbArgs& a, Context& ctx) -> Result<json::Value> {
        auto& app = ctx.service<EngineApp>();
        // Plain float WAVs of the two mixes: no delivery processing.
        Args opts;
        opts.settings.startBeat = a.startBeat;
        opts.settings.endBeat = a.endBeat;
        opts.settings.tailS = a.tailS;
        opts.settings.outputs = std::vector<rs::Output>{rs::Output{}};
        opts.settings.outputs->front().bitDepth = 32;
        opts.settings.normalize = rs::Normalize{};
        opts.settings.trim = rs::Trim{};
        opts.settings.seamlessLoop = false;
        opts.settings.ifExists = rs::IfExists::Overwrite;
        opts.settings.channels = 2;
        opts.settings.report = false;
        opts.settings.addToProject = false;
        opts.settings.metadata = rs::Metadata{};
        opts.settings.limitTruePeakDb.reset();
        if (!a.startBeat && !a.endBeat) opts.settings.bounds = rs::Bounds::Project;
        std::vector<fs::path> paths;
        std::vector<json::Value> results;
        for (const auto& [branch, file] : {std::pair{a.a, "ab-a"}, std::pair{a.b, "ab-b"}}) {
            opts.sel.name = file;
            opts.sel.branch = branch;
            auto planned = plan(app, rs::Source::Mix, opts);
            if (!planned) return std::move(planned).error();
            RunContext run{app};
            auto result = runTasks(run, *planned);
            if (!result) return std::move(result).error();
            paths.push_back(planned->tasks.at(0).outputs.front().path);
            results.push_back(std::move(*result));
        }
        auto left = audio::readAudio(paths[0]);
        if (!left) return std::move(left).error();
        auto right = audio::readAudio(paths[1]);
        if (!right) return std::move(right).error();
        analysis_commands::Loaded la, lb;
        la.data = std::move(*left);
        lb.data = std::move(*right);
        auto ar = analysis_commands::analyze(la), br = analysis_commands::analyze(lb);
        if (!ar) return std::move(ar).error();
        if (!br) return std::move(br).error();
        const auto comparison = analysis_commands::compare(*ar, *br);
        json::Value out = json::Value::object();
        out["a"] = results[0].find("files")->at(0).find("render")->asString();
        out["b"] = results[1].find("files")->at(0).find("render")->asString();
        json::Value report = json::Value::object();
        report["notable"] = comparison.notable;
        report["ltas_rms_db"] = comparison.ltasRmsDb;
        report["summary"] = comparison.summary;
        out["comparison"] = std::move(report);
        return out;
    };
    registry.add(std::move(ab));
    registerQueueCommands(registry);
    registerSettingsCommands(registry);
}
}  // namespace supple::engine
