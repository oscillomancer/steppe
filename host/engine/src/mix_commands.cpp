// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// item.mix-to-track: the selected items through their channels' full strips,
// summed and decoded as a fresh neutral buss would hear them, rendered in a
// background job onto a new muted channel (one undo step).
#include <chrono>
#include <functional>
#include <set>

#include "command_validation.h"
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
struct Commit {
    std::vector<Id> items;
    std::string expected;
    double start = 0, end = 0;
    model::MediaSource media;
};
auto argsSchema() {
    return schema::object<Args>("item_mix_to_track_args")
        .field("items", &Args::items, schema::arrayOf(model::idSchema()).minItems(1))
        .build();
}
auto commitSchema() {
    return schema::object<Commit>("item_mix_to_track_commit")
        .field("items", &Commit::items, schema::arrayOf(model::idSchema()).minItems(1))
        .field("expected", &Commit::expected, schema::string())
        .field("start", &Commit::start, schema::number())
        .field("end", &Commit::end, schema::number())
        .field("media", &Commit::media, model::schemaOf<model::MediaSource>())
        .build();
}
Error stale() {
    return Error("item.mix_stale", "the selected items changed during the mix", "/items",
                 "run item.mix-to-track again with the current selection");
}

struct Plan {
    std::set<Id> tracks;  // the source channels
    double start = 0, end = 0;
};
// The selection's channels and span; every item must be on a channel, not
// frozen, and at least one must have material.
Result<Plan> planMix(const model::Session& s, const std::vector<Id>& items) {
    Plan plan;
    bool first = true, material = false;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const std::string path = "/items/" + std::to_string(i);
        const model::Item* it = s.items.find(items[i]);
        if (!it)
            return Error("item.unknown", "no " + items[i].str(), path,
                         "use ids from session.query {kind: \"item\"}");
        const model::Track* track = s.tracks.find(it->track);
        if (!track || track->type != model::StripType::Channel)
            return Error("item.mix_not_channel", items[i].str() + " is not on a channel", path,
                         "select items on channels only");
        if (auto owner = model::frozenOwner(s, items[i]))
            return model::frozenError(s, *owner, path);
        const model::Take* take = it->activeTake ? s.takes.find(*it->activeTake) : nullptr;
        material = material || (take && take->source && !it->mute);
        const auto span = model::itemSpan(s, *it);
        plan.start = first ? span.startSec : std::min(plan.start, span.startSec);
        plan.end = first ? span.endSec : std::max(plan.end, span.endSec);
        first = false;
        plan.tracks.insert(it->track);
    }
    if (!material)
        return Error("item.mix_empty", "none of the selected items plays any material", "/items",
                     "select items with an unmuted take that has audio or MIDI");
    if (!(plan.end > plan.start))
        return Error("item.mix_empty", "the selected items span no time", "/items",
                     "select items with a length");
    return plan;
}

// The session as the mix hears it: a fresh neutral buss under the main master
// is every source channel's parent; no mute switch or solo anywhere (VCA ones
// included); only the selected items exist. Its pre-FX tap is the decoded sum.
Result<std::pair<model::Session, Id>> mixView(const model::Session& s, const Plan& plan,
                                              const std::set<Id>& selected) {
    model::EditTxn view(s, s.version);
    model::Track buss;
    buss.type = model::StripType::Buss;
    buss.name = "mix";
    buss.parent = model::kMaster;
    buss.children.assign(plan.tracks.begin(), plan.tracks.end());
    const Id bussId = view.create(std::move(buss));
    view.modify<model::Track>(model::kMaster,
                              [&](model::Track& t) { t.children.push_back(bussId); });
    for (const Id id : plan.tracks) {
        const Id parent = *s.tracks.find(id)->parent;
        view.modify<model::Track>(parent, [&](model::Track& t) {
            t.children.erase(std::remove(t.children.begin(), t.children.end(), id),
                             t.children.end());
        });
        view.modify<model::Track>(id, [&](model::Track& t) {
            t.parent = bussId;
            t.parentSend.enabled = true;
        });
    }
    for (const auto& e : s.tracks)
        if (e.value->mute || e.value->solo != model::Solo::Off)
            view.modify<model::Track>(e.id, [](model::Track& t) {
                t.mute = false;
                t.solo = model::Solo::Off;
            });
    for (const auto& e : s.groups)
        if (e.value->vca && (e.value->vca->mute || e.value->vca->solo))
            view.modify<model::TrackGroup>(
                e.id, [](model::TrackGroup& g) { g.vca->mute = g.vca->solo = false; });
    auto snapshot = view.commit();
    if (!snapshot) return snapshot.error();
    // A private view: unselected items are left out without deleting what they own.
    model::IdMap<model::Item>::Builder items(snapshot->items);
    for (const auto& e : snapshot->items)
        if (!selected.contains(e.id)) items.erase(e.id);
    snapshot->items = items.build();
    return std::pair{std::move(*snapshot), bussId};
}

struct Artifact {
    fs::path directory, file;
    bool published = false;
    ~Artifact() {
        std::error_code ec;
        if (!published && !file.empty()) fs::remove(file, ec);
        fs::remove(directory, ec);
        fs::remove(directory.parent_path(), ec);  // only removes an empty pending directory
    }
};
struct Work {
    model::Session snapshot;
    Id buss;
    Commit commit;
    RenderTarget target;
    fs::path projectDir, mediaDir;
    std::shared_ptr<Artifact> artifact;
    bool named = false;
};

Status render(Work& work, const std::vector<host::PluginInfo>& catalogue,
              const host::HostIdentity& identity, cmd::JobManager& jobs, const std::string& job,
              StopToken stop) {
    OfflineSettings settings;
    settings.applyMasterPlayrate = false;  // the result plays under the master rate itself
    settings.projectDir = work.projectDir;
    settings.track = work.buss;
    settings.tap = model::Tap::PreFx;
    settings.startSeconds = work.commit.start;
    settings.endSeconds = work.commit.end;
    settings.stopToken = stop;
    settings.cancelFlag = jobs.cancellationFlag(job);
    settings.progress = [&jobs, &job](double progress) {
        (void)jobs.progress(job, progress, "Mixing items");
    };
    auto audio = renderSnapshot(work.snapshot, settings, catalogue, identity);
    if (!audio) return audio.error();
    if (!audio->warnings.empty())
        return Error("item.mix_render", audio->warnings.front(), "/items",
                     "repair missing media or plug-ins on the source channels, then retry");
    for (const auto& channel : audio->audio.channels)
        for (float value : channel)
            if (!std::isfinite(value))
                return Error("item.mix_render", "nonfinite audio output", "/items",
                             "repair the plug-in or source producing it, then retry");
    std::error_code ec;
    fs::create_directories(work.artifact->directory, ec);
    if (ec)
        return Error("item.mix_media", ec.message(), "/items",
                     "choose a writable project media folder");
    const auto name = work.artifact->directory.filename().string() + ".wav";
    work.artifact->file = work.artifact->directory / name;
    SUPPLE_TRY(audio::writeWav(work.artifact->file, audio->audio));
    auto hash = sha256File(work.artifact->file);
    if (!hash) return hash.error();
    work.commit.media = {work.named ? name : (work.mediaDir / name).string(),
                         *hash,
                         "wav",
                         audio->audio.sampleRate,
                         static_cast<int>(audio->audio.channelCount()),
                         static_cast<std::int64_t>(audio->audio.frames())};
    return {};
}

Result<json::Value> publish(EngineApp& app, cmd::Dispatcher& dispatcher, Work& work,
                            const cmd::Origin& origin) {
    SUPPLE_TRY(checkRenderTarget(app, work.target));
    if (app.mediaDir() != work.mediaDir ||
        model::itemEditFingerprint(app.view(), work.commit.items) != work.commit.expected)
        return stale();
    std::error_code ec;
    const auto output = work.mediaDir / work.artifact->file.filename();
    if (fs::exists(output, ec))
        return Error("item.mix_media", "output filename is already in use", "/items",
                     "run item.mix-to-track again to reserve a new name");
    fs::rename(work.artifact->file, output, ec);
    if (ec)
        return Error("item.mix_media", ec.message(), "/items",
                     "check media-folder permissions and free space");
    work.artifact->file = output;
    auto committed = dispatcher.execute(
        {"item.mix-to-track.commit", commitSchema().encode(work.commit), origin});
    if (committed) work.artifact->published = true;
    return committed;
}

// Where the new channel goes: after the last source in arrange order, under
// their common parent, else under the main master after the top-level strip
// holding the last source (at its end when that is under another master).
std::pair<Id, std::size_t> placement(const model::Session& s, const std::set<Id>& sources) {
    std::vector<Id> order;
    std::function<void(Id)> walk = [&](Id id) {
        order.push_back(id);
        for (Id child : s.tracks.find(id)->children) walk(child);
    };
    for (Id master : s.project->masters) walk(master);
    Id last = *sources.begin();
    for (Id id : order)
        if (sources.contains(id)) last = id;
    std::set<Id> parents;
    for (Id id : sources) parents.insert(*s.tracks.find(id)->parent);
    auto after = [&](Id parent, Id child) {
        const auto& kids = s.tracks.find(parent)->children;
        return std::pair{parent,
                         static_cast<std::size_t>(std::find(kids.begin(), kids.end(), child) -
                                                  kids.begin() + 1)};
    };
    if (parents.size() == 1) return after(*parents.begin(), last);
    for (Id up = last; s.tracks.find(up)->parent;) {
        const Id parent = *s.tracks.find(up)->parent;
        if (parent == model::kMaster) return after(model::kMaster, up);
        up = parent;
    }
    return {model::kMaster, s.tracks.find(model::kMaster)->children.size()};
}
}  // namespace

void registerMixCommands(cmd::Registry& registry) {
    cmd::Command<Commit, json::Value> commit;
    commit.name = "item.mix-to-track.commit";
    commit.kind = cmd::Kind::Edit;
    commit.exposure = cmd::Exposure::Internal;
    commit.summary = "Put a finished item mix on a new muted channel as one replayable edit.";
    commit.args = commitSchema();
    commit.result = schema::any();
    commit.run = [](const Commit& a, cmd::Context& ctx) -> Result<json::Value> {
        auto& app = ctx.service<EngineApp>();
        const auto s = app.view();
        auto plan = planMix(s, a.items);
        if (!plan) return plan.error();
        if (model::itemEditFingerprint(s, a.items) != a.expected) return stale();
        const auto& media = a.media;
        const auto frames = time::samplesAt(a.end, s.project->sampleRate) -
                            time::samplesAt(a.start, s.project->sampleRate);
        if (media.sampleRate != s.project->sampleRate || media.channels != 2 ||
            media.frames != frames)
            return Error("item.mix_media", "media does not cover the selected span", "/media",
                         "run item.mix-to-track again");
        auto path = fs::path(media.path);
        if (path.is_relative()) path = app.mediaDir() / path;
        auto hash = sha256File(path);
        if (!hash) return hash.error();
        if (*hash != media.hash)
            return Error("item.mix_media", "the mixed file changed", "/media",
                         "restore the mixed file or run item.mix-to-track again");
        // One source channel names the result after itself; several make "Mix".
        std::string name = "Mix";
        if (plan->tracks.size() == 1) name = model::trackLabel(s, *plan->tracks.begin()) + " mix";
        const auto [parent, index] = placement(s, plan->tracks);
        auto add = json::Value::object();
        add["type"] = "channel";
        add["name"] = name;
        add["parent"] = parent.str();
        add["index"] = static_cast<std::int64_t>(index);
        auto added = ctx.service<const cmd::Registry>().find("track.add")->invoke(add, ctx);
        if (!added) return added.error();
        const Id track = *Id::parse((*added)["track"].asString());
        app.txn().modify<model::Track>(track, [](model::Track& t) { t.mute = true; });
        const auto now = app.view();
        model::Take take;
        take.name = name;
        take.source = app.txn().create(media);
        const Id fresh = app.txn().create(std::move(take));
        model::Item item;
        item.track = track;
        item.takes = {fresh};
        item.activeTake = fresh;
        model::placeItemSeconds(now, item, a.start, a.end);
        const Id made = app.txn().create(std::move(item));
        auto out = json::Value::object();
        out["track"] = track.str();
        out["item"] = made.str();
        out["file"] = path.string();
        return out;
    };
    addChecked(registry, std::move(commit));

    cmd::Command<Args, json::Value> command;
    command.name = "item.mix-to-track";
    command.kind = cmd::Kind::Action;
    command.exposure = cmd::Exposure::McpTool;
    command.summary = "Mix the selected items through their strips onto a new muted channel.";
    command.description =
        "Background job. Only the given items sound; each plays through its channel's full strip "
        "(take FX, item gain/fades, track FX, automation, fader, pan, Console 9 encode), and the "
        "sum is decoded by a neutral Console 9 Buss, as a fresh buss fed by those channels hears "
        "it. Sends, parent busses, mute switches and solo are ignored. Span: earliest item start "
        "to latest end, no tail. Result: a project-rate stereo float WAV in the media folder on a "
        "new muted channel after the last source (under their common parent, else the main "
        "master), one item at the span's start. Source items stay. One undo step; job.get "
        "returns track, item and file.";
    command.args = argsSchema();
    command.result = schema::any();
    command.errors = {
        {"item.mix_not_channel", "an item is not on a channel", "select items on channels only"},
        {"item.mix_empty", "nothing selected plays material", "select items with audio or MIDI"},
        {"track.frozen", "an item is on a frozen channel", "unfreeze the channel first"},
        {"item.mix_*", "stale selection, failed render or media", "repair and retry the mix"}};
    command.examples = {{"Mix an audio item to a new channel",
                         *json::parse(R"({"items":["it_1"]})"), *json::parse(R"({"job":"<any>"})"),
                         "song_with_audio"}};
    command.run = [](const Args& a, cmd::Context& ctx) -> Result<json::Value> {
        auto& app = ctx.service<EngineApp>();
        const auto s = app.view();
        auto plan = planMix(s, a.items);
        if (!plan) return plan.error();
        auto plugins = app.plugins();
        if (!plugins) return plugins.error();
        auto view = mixView(s, *plan, std::set<Id>(a.items.begin(), a.items.end()));
        if (!view) return view.error();
        Work work;
        work.snapshot = std::move(view->first);
        work.buss = view->second;
        work.commit.items = a.items;
        work.commit.expected = model::itemEditFingerprint(s, a.items);
        work.commit.start = plan->start;
        work.commit.end = plan->end;
        work.target = renderTarget(app);
        work.projectDir = app.projectDir();
        work.mediaDir = app.mediaDir();
        work.named = !app.projectPath().empty();
        static std::atomic<std::uint64_t> serial{0};
        const auto name =
            "mix-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(serial.fetch_add(1));
        work.artifact = std::make_shared<Artifact>();
        work.artifact->directory = work.mediaDir / ".mix-pending" / name;
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
                "mix-to-track.complete",
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
