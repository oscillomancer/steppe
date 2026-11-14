// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Project commands: project.* and session.describe (session-model §7).
#include "command_helpers.h"

namespace supple::engine {

namespace {

// --- project / session -----------------------------------------------------------------

struct ProjectTimecode {
    std::string rate, start;  // as project.timecode saves them, defaults included
};

struct ProjectInfo {
    std::string name, path, branch;
    double sampleRate = 0, tempo = 0;
    std::string meter;
    ProjectTimecode timecode;
    std::int64_t version = 0;
    std::int64_t generation = 0;
    std::int64_t recoverable = 0;    // recordings project.recover can bring back
    bool autosaveAvailable = false;  // a newer autosave exists (not loaded)
    std::optional<std::vector<schema::Warning>> warnings;
};

schema::Schema<schema::Warning> warningSchema(std::string_view title) {
    return schema::object<schema::Warning>(std::string(title))
        .field("code", &schema::Warning::code, schema::string())
        .field("message", &schema::Warning::message, schema::string())
        .field("path", &schema::Warning::path, schema::string())
        .build();
}

schema::Schema<ProjectInfo> projectInfoSchema() {
    return schema::object<ProjectInfo>("project_info")
        .field("name", &ProjectInfo::name, schema::string())
        .field("path", &ProjectInfo::path, schema::string())
        .field("branch", &ProjectInfo::branch, schema::string())
        .field("sample_rate", &ProjectInfo::sampleRate, schema::number())
        .field("tempo", &ProjectInfo::tempo, schema::number())
        .field("meter", &ProjectInfo::meter, schema::string())
        .field("timecode", &ProjectInfo::timecode,
               schema::object<ProjectTimecode>("project_timecode")
                   .field("rate", &ProjectTimecode::rate, schema::string())
                   .field("start", &ProjectTimecode::start, schema::string())
                   .build()
                   .doc("The timecode frame rate and the label at project time 0"))
        .field("version", &ProjectInfo::version, schema::integer())
        .field("generation", &ProjectInfo::generation, schema::integer())
        .field("recoverable", &ProjectInfo::recoverable, schema::integer())
        .field("autosave_available", &ProjectInfo::autosaveAvailable, schema::boolean())
        .field("warnings", &ProjectInfo::warnings, schema::arrayOf(warningSchema("load_warning")))
        .build();
}

ProjectInfo projectInfo(EngineApp& app, const model::Session& s) {
    const auto m = s.timeMap->meterAt(1);
    const fs::path project = app.projectPath();
    return ProjectInfo{s.project->name,
                       project.string(),
                       app.versions().branch(),
                       s.project->sampleRate,
                       s.timeMap->bpmAt(0),
                       std::to_string(m.numerator) + "/" + std::to_string(m.denominator),
                       {std::string(sync::rateName(s.project->timecode.rate)),
                        sync::format(s.project->timecode.start, s.project->timecode.rate)},
                       static_cast<std::int64_t>(s.version),
                       static_cast<std::int64_t>(app.projectGeneration()),
                       static_cast<std::int64_t>(app.recoverable().size()),
                       !app.newerAutosave(project).empty(),
                       std::nullopt};
}

struct RecoverableList {
    std::vector<RecoverableInfo> recordings;
    std::optional<std::vector<RecordingWarning>> warnings;
};

struct RecoverArgs {
    std::optional<std::vector<std::string>> files;
    std::optional<std::string> track;
};

struct RecoverResult {
    std::vector<RecordedInfo> recovered;
    std::optional<std::vector<RecordingWarning>> warnings;
};

struct TimecodeArgs {
    std::optional<sync::FrameRate> rate;
    std::optional<std::string> start;
};

struct ProjectSetArgs {
    std::optional<std::string> name, title, author, notes;
    std::optional<bool> console9;
    std::optional<TimecodeArgs> timecode;
    std::optional<model::PanLaw> panLaw;
    std::optional<json::Value> render;
    std::optional<model::Timebase> timebase;
    std::optional<std::uint64_t> historyBudgetBytes;
};

struct PathArgs {
    std::optional<std::string> path;
};

struct SaveResult {
    std::string path;
    std::optional<std::vector<schema::Warning>> warnings;  // plug-in states not read
};

}  // namespace

void registerProjectCommands(cmd::Registry& registry) {
    // ---------------------------------------------------------------- project.get
    [&] {
        Command<NoArgs, ProjectInfo> c;
        c.name = "project.get";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Show the project: name, rate, tempo, meter, file, version and branch.";
        c.tags = {"project"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.description =
            "recoverable: recordings a crash left out of the project (project.recoverable). "
            "autosave_available: a newer autosave exists next to the file; open it as a project "
            "if you want it (nothing is loaded silently).";
        c.result = projectInfoSchema();
        c.examples = {
            {"A new project", obj("{}"),
             obj(R"({"name": "", "path": "", "branch": "main", "sample_rate": 48000.0, "tempo": 120.0, "meter": "4/4", "timecode": {"rate": "30", "start": "00:00:00:00"}, "version": 0, "recoverable": 0, "generation": "<any>", "autosave_available": false})"),
             "empty"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<ProjectInfo> {
            EngineApp& app = A(ctx);
            return projectInfo(app, app.view());
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- project.set
    [&] {
        Command<ProjectSetArgs, Empty> c;
        c.name = "project.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Set project name, notes, Console 9, pan law, timebase or timecode.";
        c.description =
            "timebase: what items follow when the tempo changes, for items and tracks that "
            "inherit it — beats (default), time or beats_position; they keep their place now. "
            "timecode: the frame rate (23.976, 24, 25, 29.97df, 29.97 or 30; default 30) and "
            "start, the timecode at project time 0 (default 00:00:00:00), which MTC and LTC "
            "sync and output use; either member alone keeps the other.";
        c.tags = {"project"};
        // Share the model's ID metadata for merge replay, while retaining the
        // state-aware checks and repair hints in run().
        auto renderSchema = model::schemaOf<model::Project>().jsonSchema()["properties"]["render"];
        renderSchema.asObject().erase("default");  // omission leaves existing settings unchanged
        // The typed render settings (session-model §1.10); members before
        // schema 2.3 (format, bit_depth, secondary, tail_ms) are migrated.
        const schema::Schema<json::Value> renderArgs{
            "render settings",
            [](const json::Value& v, json::Value& out, schema::DecodeCtx& ctx,
               const std::string& p) {
                out = v;
                if (auto migrated = model::render::migrateLegacy(out, p); !migrated) {
                    ctx.error(migrated.error().code, migrated.error().message,
                              migrated.error().path, migrated.error().hint);
                    return false;
                }
                model::render::Settings settings;
                if (!model::render::settingsSchema().decodeInto(out, settings, ctx, p))
                    return false;
                out = model::render::toJson(settings);
                return true;
            },
            [](const json::Value& v) { return v; }, std::move(renderSchema)};
        c.args = schema::object<ProjectSetArgs>("project_set_args")
                     .field("name", &ProjectSetArgs::name, schema::string())
                     .field("title", &ProjectSetArgs::title, schema::string())
                     .field("author", &ProjectSetArgs::author, schema::string())
                     .field("notes", &ProjectSetArgs::notes, schema::string())
                     .field("console9", &ProjectSetArgs::console9, schema::boolean())
                     .field("pan_law", &ProjectSetArgs::panLaw,
                            model::panLawSchema().doc(
                                "Channels' and busses' pan law unless a strip sets its own: db "
                                "is the centre's cut (0, -2.5, -3, -4.5 or -6; default 0); "
                                "boost keeps the centre at 0 dB and raises the sides instead"))
                     .field("history_budget_bytes", &ProjectSetArgs::historyBudgetBytes,
                            schema::integerOf<std::uint64_t>().min(1).doc(
                                "Estimated retained history byte budget; default 1 GiB. Leases and "
                                "branch boundaries may exceed it."))
                     .field("render", &ProjectSetArgs::render, renderArgs)
                     .field("timebase", &ProjectSetArgs::timebase, model::timebaseSchema())
                     .field("timecode", &ProjectSetArgs::timecode,
                            schema::object<TimecodeArgs>("timecode_args")
                                .field("rate", &TimecodeArgs::rate, model::frameRateSchema())
                                .field("start", &TimecodeArgs::start,
                                       schema::string().doc(
                                           "HH:MM:SS:FF, \";\" before the frames in drop-frame"))
                                .build())
                     .build();
        c.result = emptySchema();
        c.examples = {
            {"Name it", obj(R"({"name": "Demo"})"), obj("{}"), "empty"},
            {"EBU timecode starting at one hour",
             obj(R"({"timecode": {"rate": "25", "start": "01:00:00:00"}})"), obj("{}"), "empty"},
            {"Pan with the -3 dB law, boosting the sides",
             obj(R"({"pan_law": {"db": -3, "boost": true}})"), obj("{}"), "empty"}};
        c.run = [](const ProjectSetArgs& a, Context& ctx) -> Result<Empty> {
            model::EditTxn& t = A(ctx).txn();
            model::Project p = t.project();
            if (a.name) p.name = *a.name;
            if (a.title) p.title = *a.title;
            if (a.author) p.author = *a.author;
            if (a.notes) p.notes = *a.notes;
            if (a.console9) p.console9 = *a.console9;
            if (a.panLaw) p.panLaw = *a.panLaw;
            if (a.timecode) {
                sync::TimecodeMapping tc = p.timecode;
                if (a.timecode->rate) tc.rate = *a.timecode->rate;
                if (a.timecode->start) {
                    auto label = sync::parse(*a.timecode->start, tc.rate);
                    if (!label)
                        return Error("timecode.start", label.error().message, "/timecode/start",
                                     label.error().hint);
                    tc.start = *label;
                } else if (!sync::valid(tc.start, tc.rate)) {
                    return Error("timecode.start",
                                 "the start " + sync::format(tc.start, p.timecode.rate) +
                                     " is not a timecode at " +
                                     std::string(sync::rateName(tc.rate)) + " fps",
                                 "/timecode/start",
                                 "set start with the rate, e.g. {\"rate\": \"" +
                                     std::string(sync::rateName(tc.rate)) +
                                     "\", \"start\": \"00:00:00:00\"}");
                }
                p.timecode = tc;
            }
            if (a.historyBudgetBytes) p.historyBudgetBytes = *a.historyBudgetBytes;
            if (a.render) {
                if (!a.render->isObject())
                    return Error(
                        "render.settings", "render settings must be an object", "/render",
                        "provide render as an object, for example {\"normalize\":\"none\"}");
                if (const auto* matrix = a.render->find("matrix")) {
                    if (!matrix->isObject())
                        return Error("render.matrix", "matrix must be an object", "/render/matrix",
                                     "provide matrix as an object mapping region ids or project to "
                                     "arrays of track ids");
                    for (const auto& [key, row] : matrix->asObject()) {
                        if (key != "project") {
                            auto id = model::Id::parse(key);
                            if (!id || !A(ctx).view().regions.contains(*id))
                                return Error("render.matrix", "unknown matrix region " + key,
                                             "/render/matrix", "use region.add first");
                        }
                        if (!row.isArray())
                            return Error("render.matrix", "matrix row must be an array",
                                         "/render/matrix",
                                         "replace this matrix row with an array of track/master "
                                         "ids from track.list");
                        for (const auto& cell : row.asArray()) {
                            if (!cell.isString())
                                return Error("render.matrix",
                                             "matrix cell must be a track or master id",
                                             "/render/matrix",
                                             "replace this cell with an existing track or master "
                                             "id from track.list");
                            {
                                auto id = model::Id::parse(cell.asString());
                                if (!id || !A(ctx).view().tracks.contains(*id))
                                    return Error("render.matrix",
                                                 "unknown matrix track " + cell.asString(),
                                                 "/render/matrix", "use track.list");
                            }
                        }
                    }
                }
                if (const auto* stems = a.render->find("stems"))
                    if (const auto* tracks = stems->find("tracks"))
                        for (const auto& cell : tracks->asArray()) {
                            auto id = model::Id::parse(cell.asString());
                            if (!id || !A(ctx).view().tracks.contains(*id))
                                return Error("track.unknown",
                                             "unknown stem track " + cell.asString(),
                                             "/render/stems/tracks", "use track.list");
                        }
                if (const auto* masters = a.render->find("masters"))  // ADR-0056
                    for (const auto& cell : masters->asArray()) {
                        auto id = model::Id::parse(cell.asString());
                        const model::Session view = A(ctx).view();
                        const model::Track* strip = id ? view.tracks.find(*id) : nullptr;
                        if (!strip || strip->type != model::StripType::Master)
                            return Error("render.masters", "not a master: " + cell.asString(),
                                         "/render/masters",
                                         "list master ids from track.list (type master)");
                    }
                p.render = *a.render;
            }
            if (a.timebase && *a.timebase != p.timebase) {
                // Items that inherit the project's timebase keep their place.
                const model::Session s = A(ctx).view();
                for (const auto& e : s.items) {
                    const model::Track* tr = s.tracks.find(e.value->track);
                    if (e.value->timebase != model::ItemTimebase::Track ||
                        (tr && tr->timebase != model::TrackTimebase::Project))
                        continue;
                    t.modify<model::Item>(e.id, [&](model::Item& it) {
                        model::retimeItem(s, it, p.timebase, *a.timebase);
                    });
                }
                p.timebase = *a.timebase;
            }
            t.setProject(std::move(p));
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- project.save / project.open
    [&] {
        Command<PathArgs, SaveResult> c;
        c.name = "project.save";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Save the project (canonical JSON) to its file or a new path.";
        c.description =
            "path: where to save (becomes the project's file); omit to save in place. warnings: "
            "running plug-ins whose current state could not be read; the file keeps the state "
            "each had before (fx.restart, then save again, or replace the plug-in).";
        c.tags = {"project"};
        c.args = schema::object<PathArgs>("path_args")
                     .field("path", &PathArgs::path, schema::string())
                     .build();
        c.result = schema::object<SaveResult>("save_result")
                       .field("path", &SaveResult::path, schema::string())
                       .field("warnings", &SaveResult::warnings,
                              schema::arrayOf(warningSchema("save_warning")))
                       .build();
        c.examples = {{"Save next to the work directory",
                       obj(R"({"path": "example-project.json"})"), obj(R"({"path": "<any>"})"),
                       "empty"}};
        c.run = [](const PathArgs& a, Context& ctx) -> Result<SaveResult> {
            EngineApp& app = A(ctx);
            fs::path p = a.path ? fs::path(*a.path) : app.projectPath();
            if (p.empty())
                return Error("project.no_path", "the project has no file yet", "/path",
                             "give a path");
            std::vector<schema::Warning> warnings;
            SUPPLE_TRY(app.save(p, &warnings));
            for (const auto& w : warnings) ctx.warn(w.code, w.message, w.path);
            SaveResult result{app.projectPath().string(), {}};
            if (!warnings.empty()) result.warnings = std::move(warnings);
            return result;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<PathArgs, ProjectInfo> c;
        c.name = "project.open";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Open a project file (replaces the current project and its history).";
        c.tags = {"project"};
        c.args = schema::object<PathArgs>("path_args")
                     .field("path", &PathArgs::path, schema::string())
                     .build();
        c.description =
            "recoverable > 0: recordings a crash left out of the project; project.recoverable "
            "lists them and project.recover puts them back. autosave_available: a newer autosave "
            "exists next to the file (it is not loaded silently). warnings also report project "
            "module copies that could not be read or compiled, and name types using a library "
            "version instead or left unavailable.";
        c.result = projectInfoSchema();
        c.errors = {{"file.open", "the file cannot be read", "check the path"},
                    {"schema.invalid", "the file has problems", "details list each with its path"}};
        c.examples = {
            {"Open", obj(R"({"path": "example-project.json"})"),
             obj(R"({"name": "<any>", "path": "<any>", "branch": "main", "sample_rate": "<any>", "tempo": "<any>", "meter": "<any>", "timecode": "<any>", "version": "<any>", "recoverable": 0, "generation": "<any>", "autosave_available": false})"),
             "saved"}};
        c.run = [](const PathArgs& a, Context& ctx) -> Result<ProjectInfo> {
            EngineApp& app = A(ctx);
            if (!a.path)
                return Error("project.no_path", "give the project file to open", "/path",
                             "provide path naming an existing project JSON file");
            std::vector<schema::Warning> warnings;
            SUPPLE_TRY(app.open(*a.path, &warnings));
            for (const auto& w : warnings) ctx.warn(w.code, w.message, w.path);
            auto info = projectInfo(app, app.head());
            if (!warnings.empty()) info.warnings = std::move(warnings);
            return info;
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- project.new
    [&] {
        struct NewArgs {
            std::optional<std::string> name;
            std::optional<double> sampleRate;
        };
        Command<NewArgs, ProjectInfo> c;
        c.name = "project.new";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Start a new, empty project (replaces the current one and its history).";
        c.description =
            "The new project has one master and no file until project.save {path}. Unsaved "
            "changes to the current project are lost: save first.";
        c.tags = {"project"};
        c.args = schema::object<NewArgs>("project_new_args")
                     .field("name", &NewArgs::name, schema::string())
                     .field("sample_rate", &NewArgs::sampleRate,
                            schema::number().min(8000).max(384000).doc("Hz; default 48000."))
                     .build();
        c.result = projectInfoSchema();
        c.examples = {
            {"New", obj(R"({"name": "Sketch"})"),
             obj(R"({"name": "Sketch", "path": "", "branch": "main", "sample_rate": 48000.0, "tempo": "<any>", "meter": "<any>", "timecode": "<any>", "version": 0, "recoverable": 0, "generation": "<any>", "autosave_available": false})"),
             "song"}};
        c.run = [](const NewArgs& a, Context& ctx) -> Result<ProjectInfo> {
            EngineApp& app = A(ctx);
            model::Project p;
            if (a.name) p.name = *a.name;
            if (a.sampleRate) p.sampleRate = *a.sampleRate;
            SUPPLE_TRY(app.newProject(std::move(p)));
            return projectInfo(app, app.head());
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- project.recoverable /
    // project.recover
    [&] {
        Command<NoArgs, RecoverableList> c;
        c.name = "project.recoverable";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List recordings a crash left out of the project.";
        c.description =
            "Takes recorded but never committed (the program stopped mid-recording) or never "
            "saved, found in the media folder's rec/. start_seconds is where each was "
            "recorded; seconds includes audio written after the file's last header update.";
        c.tags = {"project", "recording"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result =
            schema::object<RecoverableList>("recoverable_list")
                .field("warnings", &RecoverableList::warnings,
                       schema::arrayOf(recordingWarningSchema()))
                .field("recordings", &RecoverableList::recordings,
                       schema::arrayOf(
                           schema::object<RecoverableInfo>("recoverable_recording")
                               .field("file", &RecoverableInfo::file, schema::string())
                               .field("track", &RecoverableInfo::track, schema::string())
                               .field("start_seconds", &RecoverableInfo::startSeconds,
                                      schema::number())
                               .field("seconds", &RecoverableInfo::seconds, schema::number())
                               .field("channels", &RecoverableInfo::channels, schema::integer())
                               .build()))
                .build();
        c.examples = {{"After a crash while recording", obj("{}"),
                       obj(R"({"recordings": [{"file": "rec/Vocal-1.wav", "track": "tr_1",
                              "start_seconds": 1.0, "seconds": 1.0, "channels": 2}]})"),
                       "recording_interrupted"},
                      {"Nothing to recover", obj("{}"), obj(R"({"recordings": []})"), "empty"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<RecoverableList> {
            std::vector<RecordingWarning> warnings;
            RecoverableList out;
            out.recordings = A(ctx).recoverable(&warnings);
            if (!warnings.empty()) out.warnings = std::move(warnings);
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<RecoverArgs, RecoverResult> c;
        c.name = "project.recover";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Put recordings a crash left out back into the project (one undo step).";
        c.description =
            "files: as project.recoverable lists them (omit for all). Each becomes an item on "
            "the track it was recorded on (or track) at the position it was recorded.";
        c.tags = {"project", "recording"};
        c.args = schema::object<RecoverArgs>("recover_args")
                     .field("files", &RecoverArgs::files, schema::arrayOf(schema::string()))
                     .field("track", &RecoverArgs::track, idArg("tr", "Track"))
                     .build();
        c.result =
            schema::object<RecoverResult>("recover_result")
                .field("recovered", &RecoverResult::recovered, schema::arrayOf(recordedSchema()))
                .field("warnings", &RecoverResult::warnings,
                       schema::arrayOf(recordingWarningSchema()))
                .build();
        c.errors = {{"recover.nothing", "there is nothing to recover", "project.recoverable"},
                    {"recover.unknown_file", "a file is not a recording to recover",
                     "project.recoverable lists them"},
                    {"recover.duplicate_file", "a file is listed twice", "list each once"},
                    {"recover.no_track", "the track it was recorded on is gone", "give track"}};
        c.examples = {{"Recover everything", obj("{}"),
                       obj(R"({"recovered": [{"track": "tr_1", "item": "it_1", "take": "tk_1",
                              "file": "<any>", "seconds": 1.0, "lost_frames": 0,
                              "takes": ["tk_1"]}]})"),
                       "recording_interrupted"}};
        c.run = [](const RecoverArgs& a, Context& ctx) -> Result<RecoverResult> {
            EngineApp& app = A(ctx);
            std::optional<Id> track;
            if (a.track) {
                auto id = existing<model::Track>(app.view(), *a.track, "/track");
                if (!id) return std::move(id).error();
                track = *id;
            }
            std::vector<RecordingWarning> warnings;
            auto r = app.recover(a.files.value_or(std::vector<std::string>{}), track, &warnings);
            if (!r) return std::move(r).error();
            RecoverResult out;
            out.recovered = std::move(*r);
            if (!warnings.empty()) out.warnings = std::move(warnings);
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- session.describe
    [&] {
        Command<NoArgs, TextResult> c;
        c.name = "session.describe";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Describe the whole session in compact text (tracks, items, fx, routes).";
        c.description =
            "The quickest way to see where things are; use the IDs it shows with other commands.";
        c.tags = {"session"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = schema::object<TextResult>("text")
                       .field("text", &TextResult::text, schema::string())
                       .build();
        c.examples = {{"Describe", obj("{}"), obj(R"({"text": "<any>"})"), "song"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<TextResult> {
            return TextResult{describeSession(A(ctx).view())};
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
