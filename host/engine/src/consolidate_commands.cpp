// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <chrono>
#include "command_validation.h"
#include "heard_midi.h"
#include "render_commands.h"
#include "supple/base/sha256.h"
#include "supple/model/item_edits.h"
#include "supple/model/schemas.h"

namespace supple::engine {
namespace {
using model::Id;
namespace fs = std::filesystem;
struct Args {
    std::vector<Id> items;
};
struct Media {
    Id item;
    model::MediaSource source;
};
struct Commit {
    std::vector<Id> items;
    std::string expected;
    std::vector<Media> media;
};
auto argsSchema() {
    return schema::object<Args>("item_consolidate_args")
        .field("items", &Args::items, schema::arrayOf(model::idSchema()).minItems(1))
        .build();
}
auto commitSchema() {
    auto media = schema::object<Media>("consolidated_media")
                     .field("item", &Media::item, model::idSchema())
                     .field("source", &Media::source, model::schemaOf<model::MediaSource>())
                     .build();
    return schema::object<Commit>("item_consolidate_commit")
        .field("items", &Commit::items, schema::arrayOf(model::idSchema()).minItems(1))
        .field("expected", &Commit::expected, schema::string())
        .field("media", &Commit::media, schema::arrayOf(media))
        .build();
}
Error stale() {
    return Error("item.consolidate_stale", "selected material changed during consolidation",
                 "/items", "retry consolidation with the current selection");
}
struct Artifact {
    fs::path directory;
    std::vector<fs::path> files;
    bool published = false;
    ~Artifact() {
        std::error_code ec;
        if (!published)
            for (const auto& file : files) fs::remove(file, ec);
        fs::remove(directory, ec);
        fs::remove(directory.parent_path(), ec);  // only removes an empty pending directory
    }
};
struct Work {
    model::Session snapshot;
    std::vector<model::ItemEditGroup> groups;
    Commit commit;
    RenderTarget target;
    fs::path projectDir, mediaDir;
    std::shared_ptr<Artifact> artifact;
    std::map<fs::path, std::pair<std::uintmax_t, fs::file_time_type>> stamps;
    bool named = false;
};
Result<std::map<fs::path, std::string>> inputHashes(const Work& work) {
    std::set<Id> needed;
    for (const auto& group : work.groups) {
        if (!group.audio) continue;
        for (Id id : group.items) {
            const auto& item = *work.snapshot.items.find(id);
            const auto& take = *work.snapshot.takes.find(*item.activeTake);
            needed.insert(*item.activeTake);
            needed.insert(take.warpPeers.begin(), take.warpPeers.end());
        }
    }
    // An inactive peer still influences its bundle's phase preparation.
    std::map<fs::path, std::string> hashes;
    for (Id id : needed) {
        const auto& take = *work.snapshot.takes.find(id);
        const auto& media = *work.snapshot.media.find(*take.source);
        fs::path path = media.path;
        if (path.is_relative()) {
            const auto inMedia = work.projectDir / work.snapshot.project->mediaDir / path;
            std::error_code ec;
            path = fs::exists(inMedia, ec) ? inMedia : work.projectDir / path;
        }
        if (hashes.contains(path)) continue;
        auto hash = sha256File(path);
        if (!hash) return hash.error();
        if (!media.hash.empty() && media.hash != *hash)
            return Error("item.consolidate_source", "input media content changed", "/items",
                         "reimport the changed audio and retry consolidation");
        hashes.emplace(path, *hash);
    }
    return hashes;
}

Status render(Work& work, const std::vector<host::PluginInfo>& catalogue,
              const host::HostIdentity& identity, cmd::JobManager& jobs, const std::string& job,
              StopToken stop) {
    auto hashes = inputHashes(work);
    if (!hashes) return hashes.error();
    std::error_code ec;
    for (const auto& [path, hash] : *hashes) {
        (void)hash;
        const auto size = fs::file_size(path, ec);
        if (ec) return stale();
        const auto stamp = fs::last_write_time(path, ec);
        if (ec) return stale();
        work.stamps[path] = {size, stamp};
    }
    fs::create_directories(work.artifact->directory, ec);
    if (ec)
        return Error("item.consolidate_media", ec.message(), "/items",
                     "choose a writable project media folder");
    std::size_t count = 0;
    for (const auto& group : work.groups) {
        if (jobs.cancelled(job) || stop.stop_requested())
            return Error("job.cancelled", "consolidation cancelled", {}, "retry when ready");
        if (!group.audio) continue;
        model::EditTxn capture(work.snapshot, work.snapshot.version);
        const std::set<Id> selected(group.items.begin(), group.items.end());
        // The ordinary pre-fader tap becomes a source-only capture: all take
        // processing remains, while track processing and external inputs do not.
        capture.modify<model::Track>(group.track, [](auto& track) {
            track.chain.clear();
            track.inputChain.clear();
            track.record = {};
            track.phaseInvert = track.mono = track.mute = false;
            track.solo = model::Solo::Off;
        });
        const auto& originalTrack = *work.snapshot.tracks.find(group.track);
        std::set<Id> removedFx(originalTrack.chain.begin(), originalTrack.chain.end());
        removedFx.insert(originalTrack.inputChain.begin(), originalTrack.inputChain.end());
        for (Id fx : removedFx) capture.remove<model::FxInstance>(fx);
        std::set<Id> removedEnvelopes;
        for (const auto& env : work.snapshot.envelopes)
            if (removedFx.contains(env.value->target.object)) {
                capture.remove<model::Envelope>(env.id);
                removedEnvelopes.insert(env.id);
            }
        for (const auto& entry : work.snapshot.routes)
            if (entry.value->to.track == group.track ||
                (entry.value->to.fx && removedFx.contains(*entry.value->to.fx))) {
                capture.remove<model::Route>(entry.id);
                for (const auto& env : work.snapshot.envelopes)
                    if (env.value->target.object == entry.id) {
                        capture.remove<model::Envelope>(env.id);
                        removedEnvelopes.insert(env.id);
                    }
            }
        for (const auto& entry : work.snapshot.autoItems)
            if (removedEnvelopes.contains(entry.value->envelope))
                capture.remove<model::AutomationItem>(entry.id);
        auto snapshot = capture.commit();
        if (!snapshot) return snapshot.error();
        // Like the renderer's upstream-track projection, this private view can
        // omit items without deleting their owned objects from the project.
        model::IdMap<model::Item>::Builder items(snapshot->items);
        for (const auto& entry : snapshot->items)
            if (entry.value->track == group.track && !selected.contains(entry.id))
                items.erase(entry.id);
        snapshot->items = items.build();
        OfflineSettings settings;
        settings.applyMasterPlayrate = false;
        settings.projectDir = work.projectDir;
        settings.track = group.track;
        settings.tap = model::Tap::PreFader;
        settings.startSeconds = group.start;
        settings.endSeconds = group.end;
        settings.threads = 1;
        settings.stopToken = stop;
        settings.cancelFlag = jobs.cancellationFlag(job);
        settings.progress = [&jobs, &job](double progress) {
            (void)jobs.progress(job, progress, "Consolidating items");
        };
        auto audio = renderSnapshot(*snapshot, settings, catalogue, identity);
        if (!audio) return audio.error();
        if (!audio->warnings.empty())
            return Error("item.consolidate_render", audio->warnings.front(), "/items",
                         "repair missing media/plugins or unsupported source modes, then retry");
        for (const auto& channel : audio->audio.channels)
            for (float value : channel)
                if (!std::isfinite(value))
                    return Error("item.consolidate_render", "nonfinite audio output", "/items",
                                 "repair the affected take FX or audio source");
        const auto name =
            work.artifact->directory.filename().string() + "-" + std::to_string(++count) + ".wav";
        const auto pending = work.artifact->directory / name;
        work.artifact->files.push_back(pending);
        SUPPLE_TRY(audio::writeWav(pending, audio->audio));
        auto hash = sha256File(pending);
        if (!hash) return hash.error();
        work.commit.media.push_back(
            {group.items.front(),
             {work.named ? name : (work.mediaDir / name).string(), *hash, "wav",
              audio->audio.sampleRate, static_cast<int>(audio->audio.channelCount()),
              static_cast<std::int64_t>(audio->audio.frames())}});
    }
    auto after = inputHashes(work);
    if (!after) return after.error();
    if (*after != *hashes) return stale();
    for (const auto& [path, hash] : *after) {
        (void)hash;
        const auto size = fs::file_size(path, ec);
        if (ec) return stale();
        const auto stamp = fs::last_write_time(path, ec);
        if (ec) return stale();
        if (work.stamps.at(path) != std::pair{size, stamp}) return stale();
    }
    return {};
}
Result<json::Value> publish(EngineApp& app, cmd::Dispatcher& dispatcher, Work& work,
                            const cmd::Origin& origin) {
    SUPPLE_TRY(checkRenderTarget(app, work.target));
    if (app.mediaDir() != work.mediaDir ||
        model::itemEditFingerprint(app.view(), work.commit.items) != work.commit.expected)
        return stale();
    SUPPLE_TRY(model::completeItemSelection(app.view(), work.commit.items));
    std::error_code ec;
    for (const auto& [path, stamp] : work.stamps) {
        if (fs::file_size(path, ec) != stamp.first || ec) return stale();
        if (fs::last_write_time(path, ec) != stamp.second || ec) return stale();
    }
    for (auto& pending : work.artifact->files) {
        const auto output = work.mediaDir / pending.filename();
        if (fs::exists(output, ec))
            return Error("item.consolidate_media", "output filename is already in use", "/items",
                         "retry consolidation to reserve a new name");
        fs::rename(pending, output, ec);
        if (ec)
            return Error("item.consolidate_media", ec.message(), "/items",
                         "check media-folder permissions and free space");
        pending = output;
    }
    auto committed =
        dispatcher.execute({"item.consolidate.commit", commitSchema().encode(work.commit), origin});
    if (committed) work.artifact->published = true;
    return committed;
}
}  // namespace

void registerConsolidateCommands(cmd::Registry& registry) {
    cmd::Command<Commit, json::Value> commit;
    commit.name = "item.consolidate.commit";
    commit.kind = cmd::Kind::Edit;
    commit.exposure = cmd::Exposure::Internal;
    commit.summary = "Attach consolidated audio and merge MIDI as one replayable edit.";
    commit.args = commitSchema();
    commit.result = schema::any();
    commit.run = [](const Commit& a, cmd::Context& ctx) -> Result<json::Value> {
        auto& app = ctx.service<EngineApp>();
        const auto s = app.view();
        auto groups = model::consolidationGroups(s, a.items);
        if (!groups) return groups.error();
        if (model::itemEditFingerprint(s, a.items) != a.expected) return stale();
        auto out = json::Value::object();
        out["items"] = json::Value::array();
        out["files"] = json::Value::array();
        for (const auto& group : *groups) {
            const Id base = group.items.front();
            if (!group.audio) {
                if (group.items.size() > 1 || group.layerTakes)
                    SUPPLE_TRY(detail::mergeMidi(app.view(), app.txn(), group.items));
                out["items"].push_back(base.str());
                continue;
            }
            const auto found = std::find_if(a.media.begin(), a.media.end(),
                                            [&](const auto& value) { return value.item == base; });
            if (found == a.media.end())
                return Error("item.consolidate_media", "missing consolidated media", "/media",
                             "run item.consolidate again");
            const auto& media = found->source;
            const auto frames = time::samplesAt(group.end, s.project->sampleRate) -
                                time::samplesAt(group.start, s.project->sampleRate);
            if (media.sampleRate != s.project->sampleRate || media.channels != 2 ||
                media.frames != frames)
                return Error("item.consolidate_media", "media does not cover the selected span",
                             "/media", "run item.consolidate again");
            auto path = fs::path(media.path);
            if (path.is_relative()) path = app.mediaDir() / path;
            auto hash = sha256File(path);
            if (!hash) return hash.error();
            auto info = audio::inspect(path);
            if (!info) return info.error();
            if (*hash != media.hash || info->truncated || info->frames != media.frames ||
                info->sampleRate != media.sampleRate || info->channels != media.channels)
                return Error("item.consolidate_media", "consolidated file changed", "/media",
                             "restore the original consolidated file or consolidate again");
            auto item = *s.items.find(base);
            for (Id id : group.items) {
                for (Id take : s.items.find(id)->takes)
                    SUPPLE_TRY(removeTakeObjects(app.txn(), app.view(), take));
                if (id != base) app.txn().remove<model::Item>(id);
            }
            model::Take take;
            take.source = app.txn().create(media);
            const Id fresh = app.txn().create(std::move(take));
            item.takes = {fresh};
            item.activeTake = fresh;
            item.loop = item.mute = false;
            item.gainDb = 0;
            item.fadeIn = {};
            item.fadeOut = {};
            model::placeItemSeconds(s, item, group.start, group.end);
            app.txn().put(base, std::move(item));
            out["items"].push_back(base.str());
            out["files"].push_back(path.string());
        }
        return out;
    };
    addChecked(registry, std::move(commit));

    cmd::Command<Args, json::Value> command;
    command.name = "item.consolidate";
    command.kind = cmd::Kind::Action;
    command.exposure = cmd::Exposure::McpTool;
    command.summary = "Consolidate selected audio and MIDI per track in a background job.";
    command.description =
        "Partitions by track and active source kind. Audio bakes active takes, item controls, "
        "warp and take FX/automation to float WAV; track processing remains live. MIDI items "
        "on a track become one item (each item's heard take, gaps empty, overlaps layered); a "
        "lone MIDI item with several MIDI takes layers them into one take; other lone MIDI "
        "stays unchanged. Select complete linked/warp bundles on "
        "matching lanes/groups. One undo step after successful job publication. No item-count "
        "cap; existing render/source sample budgets apply. job.get returns resulting items/files.";
    command.args = argsSchema();
    command.result = schema::any();
    command.errors = {{"item.consolidate_*", "invalid material, stale inputs or failed capture",
                       "repair the selected sources/layout and retry consolidation"},
                      {"item.selection", "selection omits linked peers",
                       "select the full group or suspend its edit link"},
                      {"warp.peers", "incomplete or invalid warp bundle",
                       "select every bundle owner or detach it first"}};
    command.examples = {{"Consolidate the active audio take", *json::parse(R"({"items":["it_1"]})"),
                         *json::parse(R"({"job":"<any>"})"), "song_with_audio"}};
    command.run = [](const Args& a, cmd::Context& ctx) -> Result<json::Value> {
        auto& app = ctx.service<EngineApp>();
        auto groups = model::consolidationGroups(app.view(), a.items);
        if (!groups) return groups.error();
        auto plugins = app.plugins();
        if (!plugins) return plugins.error();
        Work work;
        work.snapshot = app.view();
        work.groups = std::move(*groups);
        work.commit.items = a.items;
        work.commit.expected = model::itemEditFingerprint(work.snapshot, a.items);
        work.target = renderTarget(app);
        work.projectDir = app.projectDir();
        work.mediaDir = app.mediaDir();
        work.named = !app.projectPath().empty();
        static std::atomic<std::uint64_t> serial{0};
        const auto name =
            "consolidate-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(serial.fetch_add(1));
        work.artifact = std::make_shared<Artifact>();
        work.artifact->directory = work.mediaDir / ".consolidate-pending" / name;
        auto& dispatcher = ctx.service<cmd::Dispatcher>();
        auto& jobs = ctx.service<cmd::JobManager>();
        const auto job = jobs.create();
        std::vector<host::PluginInfo> catalogue(plugins->begin(), plugins->end());
        app.startJob([&app, &dispatcher, &jobs, job, work = std::move(work),
                      catalogue = std::move(catalogue), identity = app.options().identity,
                      origin = ctx.origin()](StopToken stop) mutable {
            auto rendered = render(work, catalogue, identity, jobs, job, stop);
            if (!rendered) {
                if (!jobs.cancelled(job)) (void)jobs.fail(job, rendered.error());
                return;
            }
            dispatcher.post(
                "consolidate.complete",
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
        return out;
    };
    registry.add(std::move(command));
}
}  // namespace supple::engine
