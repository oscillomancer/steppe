// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "freeze_commands.h"
#include <chrono>
#include "command_validation.h"
#include "render_commands.h"
#include "supple/base/files.h"
#include "supple/base/sha256.h"
#include "supple/model/freeze.h"
#include "supple/model/schemas.h"

namespace supple::engine {
namespace {
using model::Id;
struct Args {
    Id track;
    bool job = false;
};
struct State {
    Id fx;
    model::FxInstance value;
    std::optional<std::string> blob;
};
struct Commit {
    Id track;
    std::string expected;
    model::MediaSource media;
    model::TrackFreeze freeze;
    std::vector<State> states;
};
auto stateSchema() {
    return schema::object<State>("freeze_fx_state")
        .field("fx", &State::fx, model::idSchema())
        .field("value", &State::value, model::schemaOf<model::FxInstance>())
        .field("blob", &State::blob, schema::string())
        .build();
}
auto commitSchema() {
    return schema::object<Commit>("track_freeze_commit")
        .field("track", &Commit::track, model::idSchema())
        .field("expected", &Commit::expected, schema::string())
        .field("media", &Commit::media, model::schemaOf<model::MediaSource>())
        .field("freeze", &Commit::freeze, model::trackFreezeSchema())
        .field("states", &Commit::states, schema::arrayOf(stateSchema()))
        .build();
}
json::Value result(Id track, bool frozen) {
    auto value = json::Value::object();
    value["track"] = track.str();
    value["frozen"] = frozen;
    return value;
}
// Pending artifacts have one owner; published media is retained for history and saves.
struct Artifact {
    std::filesystem::path path;
    bool published = false;
    ~Artifact() {
        if (!published) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
            if (path.parent_path().filename() == ".freeze-pending")
                std::filesystem::remove(path.parent_path(), ec);
        }
    }
};
struct Work {
    model::Session snapshot;
    Commit commit;
    OfflineSettings settings;
    RenderTarget target;
    std::filesystem::path directory, output;
    std::shared_ptr<Artifact> artifact;
    bool named = false;
    std::int64_t shift = 0;
    std::map<std::filesystem::path, std::string> inputs;
};

Result<std::map<std::filesystem::path, std::string>> inputHashes(
    const model::Session& s, Id track, const std::filesystem::path& projectDir) {
    std::map<std::filesystem::path, std::string> hashes;
    for (const auto& item : s.items) {
        if (item.value->track != track) continue;
        for (Id takeId : item.value->takes) {
            const auto* take = s.takes.find(takeId);
            if (!take || !take->source) continue;
            const auto* source = s.media.find(*take->source);
            if (!source) continue;
            std::filesystem::path path = source->path;
            if (path.is_relative()) {
                const auto inMedia = projectDir / s.project->mediaDir / path;
                path = std::filesystem::exists(inMedia) ? inMedia : projectDir / path;
            }
            if (hashes.contains(path)) continue;
            auto hash = sha256File(path);
            if (!hash) return hash.error();
            if (!source->hash.empty() && source->hash != *hash)
                return Error("freeze.stale_target", "input media content changed: " + path.string(),
                             "/track", "reimport the changed audio before freezing");
            hashes[path] = *hash;
        }
    }
    return hashes;
}

Result<Work> prepare(EngineApp& app, Id track) {
    const auto s = app.view();
    SUPPLE_TRY(model::validateFreezeInput(s, track));
    if (app.engine() && app.engine()->recording())
        return Error("freeze.recording", "stop recording before freezing", "/track",
                     "use transport.stop");
    Work work;
    work.commit.track = track;
    auto inputs = inputHashes(s, track, app.projectDir());
    if (!inputs) return inputs.error();
    work.inputs = std::move(*inputs);
    work.commit.expected = model::freezeDependencyHash(s, track);
    work.target = renderTarget(app);
    work.directory = app.mediaDir();
    work.named = !app.projectPath().empty();
    work.settings.projectDir = app.projectDir();
    work.settings.modules = app.modules();
    work.settings.freezeTrack = track;
    work.settings.tap = model::Tap::PreFader;
    work.settings.freezeTail = true;
    work.settings.threads = 1;
    double end = 0;
    for (const auto& e : s.items)
        if (e.value->track == track) end = std::max(end, model::itemSpan(s, *e.value).endSec);
    if (end <= 0)
        return Error("freeze.empty", "track has no items", "/track",
                     "add an audio or MIDI item first");
    work.settings.endSeconds = end;
    model::EditTxn capture(s, s.version);
    for (Id fx : model::freezeFxIds(s, track)) {
        State state{fx, *s.fx.find(fx), {}};
        if (auto* live = app.instanceOf(fx); live && !live->failed()) {
            if (live->info().semantic) {
                auto patch = live->getPatch(false);
                if (!patch) return patch.error();
                auto parsed = json::parse(*patch);
                if (!parsed) return parsed.error();
                state.value.patch = *parsed;
                state.value.state.reset();
            } else {
                auto bytes = live->saveState();
                if (!bytes) return bytes.error();
                state.value.state = "sha256:" + sha256Hex(*bytes);
                state.value.patch.reset();
                state.blob = base64Encode(*bytes);
                capture.putBlob(*state.value.state,
                                std::make_shared<const model::Blob>(model::Blob{*bytes}));
            }
        }
        capture.put<model::FxInstance>(fx, state.value);
        work.commit.states.push_back(std::move(state));
    }
    auto snapshot = capture.commit();
    if (!snapshot) return snapshot.error();
    work.snapshot = std::move(*snapshot);
    work.artifact = std::make_shared<Artifact>();
    static std::atomic<std::uint64_t> serial{0};
    work.output =
        work.directory /
        ("freeze-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(serial.fetch_add(1)) + ".wav");
    work.artifact->path = work.directory / ".freeze-pending" / work.output.filename();
    return work;
}

Status render(Work& work, const std::vector<host::PluginInfo>& catalogue,
              const host::HostIdentity& identity) {
    auto rendered = renderSnapshot(work.snapshot, work.settings, catalogue, identity);
    while (!rendered && rendered.error().code == "freeze.tail_pending" &&
           work.settings.freezeTailLimitSeconds < 600.) {
        work.settings.freezeTailLimitSeconds =
            std::min(600., work.settings.freezeTailLimitSeconds * 2.);
        rendered = renderSnapshot(work.snapshot, work.settings, catalogue, identity);
    }
    if (!rendered) return rendered.error();
    if (!rendered->warnings.empty())
        return Error("freeze.render", rendered->warnings.front(), "/track",
                     "resolve render warnings before freezing");
    for (const auto& channel : rendered->audio.channels)
        for (float sample : channel)
            if (!std::isfinite(sample))
                return Error("freeze.render", "non-finite plugin output", "/track",
                             "repair or replace the affected plugin");
    std::error_code ec;
    std::filesystem::create_directories(work.artifact->path.parent_path(), ec);
    if (ec)
        return Error("freeze.media", ec.message(), work.directory.string(),
                     "choose a writable project folder");
    SUPPLE_TRY(audio::writeWav(work.artifact->path, rendered->audio));
    auto hash = sha256File(work.artifact->path);
    if (!hash) return hash.error();
    auto& c = work.commit;
    c.media = {work.named ? work.output.filename().string() : work.output.string(),
               *hash,
               "wav",
               rendered->audio.sampleRate,
               static_cast<int>(rendered->audio.channelCount()),
               static_cast<std::int64_t>(rendered->audio.frames())};
    c.freeze.source = Id{model::Kind::Media, 0};  // assigned atomically by commit
    c.freeze.tailFrames = rendered->freezeTailFrames;
    c.freeze.contentFrames = c.media.frames - c.freeze.tailFrames;
    c.freeze.latencyFrames = rendered->freezeLatency;
    c.freeze.dependencyHash = model::freezeDependencyHash(work.snapshot, c.track);
    work.shift = rendered->freezeShift;
    return {};
}

Result<json::Value> publish(EngineApp& app, cmd::Dispatcher& dispatcher, Work& work,
                            const cmd::Origin& origin) {
    if (auto valid = checkRenderTarget(app, work.target); !valid) return valid.error();
    const auto head = app.view();
    if (app.mediaDir() != work.directory || !head.tracks.contains(work.commit.track) ||
        head.tracks.find(work.commit.track)->freeze ||
        model::freezeDependencyHash(head, work.commit.track) != work.commit.expected)
        return Error("freeze.stale_target",
                     "the track's sound dependencies changed during rendering", "/track",
                     "retry track.freeze with the current track state");
    SUPPLE_TRY(model::validateFreezeInput(head, work.commit.track));
    auto inputs = inputHashes(head, work.commit.track, app.projectDir());
    if (!inputs) return inputs.error();
    if (*inputs != work.inputs)
        return Error("freeze.stale_target", "input media changed during rendering", "/track",
                     "reimport the changed audio and retry track.freeze");
    // Downstream latency can change this target's transport/automation grid even
    // without changing its own objects. Compare the prepared scheduling quantities.
    auto settings = work.settings;
    settings.freezeProbe = true;
    settings.freezeTail = false;
    settings.cancelFlag.reset();
    settings.progress = {};
    model::EditTxn prepared(head, head.version);
    for (const auto& state : work.commit.states) {
        prepared.put<model::FxInstance>(state.fx, state.value);
        if (state.value.state) {
            const auto blob = work.snapshot.blobs.find(*state.value.state);
            if (blob != work.snapshot.blobs.end()) prepared.putBlob(blob->first, blob->second);
        }
    }
    auto preparedHead = prepared.commit();
    if (!preparedHead) return preparedHead.error();
    auto probe = app.renderSession(*preparedHead, settings);
    if (!probe) return probe.error();
    if (probe->freezeShift != work.shift ||
        probe->freezeLatency != work.commit.freeze.latencyFrames)
        return Error("freeze.stale_target", "the track's latency schedule changed during rendering",
                     "/track", "retry track.freeze after latency or routing changes");
    // Keep pending renders out of Save As copies until the model can attach them.
    std::error_code ec;
    if (std::filesystem::exists(work.output, ec))
        return Error("freeze.media", "freeze filename already exists", work.output.string(),
                     "retry track.freeze to choose a new filename");
    const auto staging = work.artifact->path.parent_path();
    std::filesystem::rename(work.artifact->path, work.output, ec);
    if (ec)
        return Error("freeze.media", ec.message(), work.output.string(),
                     "check project-folder permissions and free space");
    work.artifact->path = work.output;
    std::filesystem::remove(staging, ec);  // succeeds only when no other render is pending
    auto committed =
        dispatcher.execute({"track.freeze.commit", commitSchema().encode(work.commit), origin});
    if (committed) work.artifact->published = true;
    return committed;
}
}  // namespace

void registerFreezeCommands(cmd::Registry& registry) {
    {
        cmd::Command<Commit, json::Value> c;
        c.name = "track.freeze.commit";
        c.kind = cmd::Kind::Edit;
        c.exposure = cmd::Exposure::Internal;
        c.summary = "Attach a verified freeze artifact as one replayable edit.";
        c.args = commitSchema();
        c.result = schema::any();
        c.run = [](const Commit& a, cmd::Context& ctx) -> Result<json::Value> {
            auto& app = ctx.service<EngineApp>();
            auto s = app.view();
            SUPPLE_TRY(model::validateFreezeInput(s, a.track));
            if (s.tracks.find(a.track)->freeze ||
                model::freezeDependencyHash(s, a.track) != a.expected)
                return Error("freeze.stale_target", "freeze dependencies changed", "/track",
                             "render the current track again");
            const auto owned = model::freezeFxIds(s, a.track);
            if (a.states.size() != owned.size())
                return Error("freeze.state", "incomplete saved FX states", "/states",
                             "use track.freeze to capture the whole chain");
            auto path = std::filesystem::path(a.media.path);
            if (path.is_relative()) path = app.mediaDir() / path;
            auto hash = sha256File(path);
            if (!hash) return hash.error();
            if (*hash != a.media.hash)
                return Error("freeze.media_missing", "frozen file content changed", "/media",
                             "restore the original freeze file");
            auto info = audio::inspect(path);
            if (!info) return info.error();
            if (info->truncated || info->frames != a.media.frames ||
                info->channels != a.media.channels || info->sampleRate != a.media.sampleRate)
                return Error("freeze.media", "frozen file metadata does not match", "/media",
                             "use track.freeze to render new media");
            for (std::size_t i = 0; i < owned.size(); ++i) {
                const auto& state = a.states[i];
                if (state.fx != owned[i])
                    return Error("freeze.state", "FX ownership changed", "/states",
                                 "freeze the current track again");
                if (state.blob) {
                    auto bytes = base64Decode(*state.blob);
                    if (!bytes) return bytes.error();
                    if (state.value.state != "sha256:" + sha256Hex(*bytes))
                        return Error("freeze.state", "state blob hash mismatch", "/states",
                                     "capture plugin state again");
                    app.txn().putBlob(*state.value.state,
                                      std::make_shared<const model::Blob>(model::Blob{*bytes}));
                }
                app.txn().put<model::FxInstance>(state.fx, state.value);
            }
            if (model::freezeDependencyHash(app.view(), a.track) != a.freeze.dependencyHash)
                return Error("freeze.state", "captured dependency hash mismatch", "/states",
                             "use track.freeze again");
            auto freeze = a.freeze;
            freeze.source = app.txn().create(a.media);
            app.txn().modify<model::Track>(a.track, [&](auto& t) { t.freeze = freeze; });
            auto out = result(a.track, true);
            out["source"] = freeze.source.str();
            out["path"] = path.string();
            out["frames"] = a.media.frames;
            out["tail_frames"] = freeze.tailFrames;
            return out;
        };
        addChecked(registry, std::move(c));
    }
    {
        cmd::Command<Args, json::Value> c;
        c.name = "track.freeze";
        c.kind = cmd::Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Freeze a channel's items and FX to pre-fader audio and release its plugins.";
        c.errors = {{"freeze.unsupported", "the channel has unsupported live dependencies",
                     "disarm it and remove external inputs; use unity master playrate"},
                    {"freeze.stale_target", "sound dependencies changed during rendering",
                     "retry track.freeze with the current sound"},
                    {"freeze.tail_limit", "the tail is infinite or did not settle",
                     "disable continuous sound or infinite feedback before freezing"}};
        c.description =
            "One undo step after rendering; job:true reports progress through job.get. "
            "Fader, pan and outgoing sends remain live. Unfreeze to edit the captured sound.";
        c.args = schema::object<Args>("track_freeze_args")
                     .field("track", &Args::track, model::idSchema())
                     .field("job", &Args::job, schema::boolean())
                     .defaultValue(false)
                     .build();
        c.result = schema::any();
        c.examples = {
            {"Freeze the channel", *json::parse(R"({"track":"tr_1"})"),
             *json::parse(
                 R"({"track":"tr_1","frozen":true,"source":"<any>","path":"<any>","frames":"<any>","tail_frames":"<any>"})"),
             "song_with_fx"}};
        c.run = [](const Args& a, cmd::Context& ctx) -> Result<json::Value> {
            auto& app = ctx.service<EngineApp>();
            if (const auto* t = app.view().tracks.find(a.track); t && t->freeze)
                return result(a.track, true);
            auto work = prepare(app, a.track);
            if (!work) return work.error();
            auto plugins = app.plugins();
            if (!plugins) return plugins.error();
            std::vector<host::PluginInfo> catalogue(plugins->begin(), plugins->end());
            const auto identity = app.options().identity;
            auto& dispatcher = ctx.service<cmd::Dispatcher>();
            if (!a.job) {
                SUPPLE_TRY(render(*work, catalogue, identity));
                return publish(app, dispatcher, *work, ctx.origin());
            }
            auto& jobs = ctx.service<cmd::JobManager>();
            const auto job = jobs.create();
            work->settings.cancelFlag = jobs.cancellationFlag(job);
            work->settings.progressFlag = jobs.progressFlag(job);
            work->settings.progress = [&jobs, job](double p) {
                (void)jobs.progress(job, p, "Freezing track");
            };
            app.startJob([&app, &dispatcher, &jobs, job, work = std::move(*work),
                          catalogue = std::move(catalogue), identity,
                          origin = ctx.origin()](supple::StopToken stop) mutable {
                work.settings.stopToken = stop;
                auto rendered = render(work, catalogue, identity);
                if (!rendered) {
                    if (!jobs.cancelled(job)) (void)jobs.fail(job, rendered.error());
                    return;
                }
                dispatcher.post(
                    "freeze.complete",
                    [&app, &dispatcher, &jobs, job, work = std::move(work),
                     origin]() mutable -> Status {
                        if (jobs.cancelled(job)) return {};
                        auto done = publish(app, dispatcher, work, origin);
                        if (!done) return jobs.fail(job, done.error());
                        return jobs.complete(job, *done);
                    },
                    cmd::Dispatcher::PostKind::Edit);
            });
            auto out = json::Value::object();
            out["job"] = job;
            out["status"] = "running";
            return out;
        };
        // Its own completion changes the head; it is deliberately not an Edit.
        registry.add(std::move(c));
    }
    {
        cmd::Command<Args, json::Value> c;
        c.name = "track.unfreeze";
        c.kind = cmd::Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Restore a frozen channel's original items and plugin state.";
        c.errors = {{"freeze.restore", "saved plugins could not be restored",
                     "install or repair the saved plugins before unfreezing"}};
        c.args = schema::object<Args>("track_unfreeze_args")
                     .field("track", &Args::track, model::idSchema())
                     .build();
        c.result = schema::any();
        c.examples = {{"Unfreeze the channel", *json::parse(R"({"track":"tr_1"})"),
                       *json::parse(R"({"track":"tr_1","frozen":false})"), "song_frozen"}};
        c.run = [](const Args& a, cmd::Context& ctx) -> Result<json::Value> {
            auto& app = ctx.service<EngineApp>();
            const auto s = app.view();
            const auto* t = s.tracks.find(a.track);
            if (!t)
                return Error("track.unknown", "track does not exist", "/track", "use track.list");
            if (!t->freeze) return result(a.track, false);
            model::EditTxn restored(s, s.version);
            restored.modify<model::Track>(a.track, [](auto& track) { track.freeze.reset(); });
            auto snapshot = restored.commit();
            if (!snapshot) return snapshot.error();
            OfflineSettings settings;
            settings.endSeconds = 1. / s.project->sampleRate;
            settings.freezeTrack = a.track;
            settings.tap = model::Tap::PreFader;
            settings.freezeProbe = true;
            auto probe = app.renderSession(*snapshot, settings);
            if (!probe) return probe.error();
            if (!probe->warnings.empty())
                return Error("freeze.restore", probe->warnings.front(), "/track",
                             "install or repair the saved plugins before unfreezing");
            app.txn().modify<model::Track>(a.track, [](auto& track) { track.freeze.reset(); });
            return result(a.track, false);
        };
        addChecked(registry, std::move(c));
    }
}
}  // namespace supple::engine
