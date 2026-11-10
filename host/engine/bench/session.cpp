// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "session.h"
#include <cmath>
#include <numbers>
#include "supple/audio/audio_file.h"
#include "supple/base/files.h"

namespace supple::engine::bench {
namespace {
json::Value object(std::string_view text) { return json::parse(text).value(); }
}  // namespace

Status buildSession(EngineApp& app, json::Value recipe, int tracks) {
    if (tracks < 1 || !recipe.isObject() || !recipe.find("schema") ||
        recipe["schema"] != json::Value("supple.capacity-session/1.0") || !recipe.find("chain") ||
        !recipe["chain"].isArray() || recipe["chain"].size() != 3 || !recipe.find("master") ||
        !recipe["master"].isObject() || !recipe.find("notation") ||
        !recipe["notation"].isString() || !recipe.find("volume_db") ||
        !recipe["volume_db"].isNumber() || !recipe.find("loop_beats") ||
        recipe["loop_beats"] != json::Value(16) || !recipe.find("sample_rate") ||
        recipe["sample_rate"] != json::Value(48000) || !recipe.find("block") ||
        recipe["block"] != json::Value(64) || !recipe.find("tempo") ||
        recipe["tempo"] != json::Value(120))
        return Error("bench.recipe", "expected a capacity-session/1.0 recipe and positive N", "",
                     "use host/engine/bench/session.json");
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, app);
    auto run = [&](std::string name, json::Value args) {
        return bus.execute({std::move(name),
                            std::move(args),
                            {cmd::Origin::Type::Internal, "capacity-bench", ""}});
    };
    auto chain = [&](const std::string& track, const json::Value& entries) -> Status {
        for (const auto& entry : entries.asArray()) {
            auto args = entry;
            args["track"] = track;
            SUPPLE_TRY(run("fx.add", std::move(args)));
        }
        return {};
    };
    for (int i = 0; i <= tracks; ++i) {
        auto args = json::Value::object();
        args["name"] = i == tracks ? "Monitored input" : "Synth " + std::to_string(i + 1);
        auto added = run("track.add", args);
        if (!added) return added.error();
        const std::string track = (*added)["track"].asString();
        args = json::Value::object();
        args["track"] = track;
        args["volume_db"] = recipe["volume_db"];
        if (i == tracks) {
            args["armed"] = true;
            args["monitor"] = "on";
            args["record_input"] = object("[1,2]");
        }
        SUPPLE_TRY(run("track.set", args));
        SUPPLE_TRY(chain(track, recipe["chain"]));
        if (i != tracks) {
            args = json::Value::object();
            args["track"] = track;
            args["notation"] = recipe["notation"];
            SUPPLE_TRY(run("item.add", args));
        }
    }
    auto master = recipe["master"];
    master["track"] = model::kMaster.str();
    SUPPLE_TRY(run("fx.add", master));
    SUPPLE_TRY(run("transport.loop", object(R"({"start_beat":0,"end_beat":16})")));
    return {};
}

Status buildEffectSession(EngineApp& app, const std::string& plugin,
                          const std::filesystem::path& input, int tracks, bool live,
                          int fxPerTrack) {
    if (tracks < 1 || tracks > 4096 || fxPerTrack < 1 || fxPerTrack > 64)
        return Error("bench.effect", "invalid track or effect count", "",
                     "use 1..4096 tracks and 1..64 effects");
    auto audio = audio::readAudio(input);
    if (!audio) return audio.error();
    if (audio->sampleRate != 48000 || audio->channels.size() != 2 || audio->frames() != 192000)
        return Error("bench.input", "expected four seconds of stereo audio at 48 kHz",
                     input.string(),
                     "generate the original fixture with tools/reaper-check/check.py::test_signal");
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, app);
    auto run = [&](std::string name, json::Value args) {
        return bus.execute({std::move(name),
                            std::move(args),
                            {cmd::Origin::Type::Internal, "capacity-bench", ""}});
    };
    for (int i = 0; i < tracks; ++i) {
        auto args = json::Value::object();
        args["name"] = "Effect " + std::to_string(i + 1);
        auto added = run("track.add", args);
        if (!added) return added.error();
        const auto track = (*added)["track"];
        args = json::Value::object();
        args["track"] = track;
        args["volume_db"] = -18;
        if (live) {
            args["armed"] = true;
            args["monitor"] = "on";
            args["record_input"] = object("[1,2]");
        }
        SUPPLE_TRY(run("track.set", args));
        args = json::Value::object();
        args["track"] = track;
        args["audio_file"] = std::filesystem::absolute(input).string();
        SUPPLE_TRY(run("item.add", args));
        for (int j = 0; j < fxPerTrack; ++j) {
            args = json::Value::object();
            args["track"] = track;
            args["plugin"] = plugin;
            SUPPLE_TRY(run("fx.add", args));
        }
    }
    SUPPLE_TRY(run("transport.loop", object(R"({"start_beat":0,"end_beat":8})")));
    return {};
}

Status isolateAll(EngineApp& app, const std::string& isolation) {
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, app);
    std::vector<model::Id> ids;
    for (const auto& entry : app.head().fx) ids.push_back(entry.id);
    for (const model::Id id : ids) {
        auto args = json::Value::object();
        args["fx"] = id.str();
        args["isolation"] = isolation;
        SUPPLE_TRY(bus.execute(
            {"fx.set", std::move(args), {cmd::Origin::Type::Internal, "capacity-bench", ""}}));
    }
    return {};
}

bool serviceTick(EngineApp& app, const Engine* expected) {
    app.tick();
    if (app.engine() != expected) return false;
    (void)app.meters();
    return true;
}

Result<json::Value> exportSession(EngineApp& app) {
    auto out = json::Value::object();
    out["schema"] = "supple.capacity-export/1.0";
    out["sample_rate"] = app.head().project->sampleRate;
    out["tempo"] = 120;
    // The centre's gain under the project's pan law (ADR-0066).
    const model::PanLaw law = app.head().project->panLaw;
    out["pan_law"] = law.boost        ? 1.0
                     : law.db == -3.0 ? std::numbers::sqrt2 / 2
                     : law.db == -6.0 ? 0.5
                                      : std::pow(10.0, law.db / 20);
    out["loop_seconds"] = app.loop().end;
    auto chain = [&](const model::Track& track) -> Result<json::Value> {
        auto list = json::Value::array();
        for (const auto id : track.chain) {
            const auto& fx = *app.head().fx.find(id);
            auto in = app.instances().get(id, fx, app.head());
            if (!in) return in.error();
            auto state = (*in)->saveState();
            if (!state) return state.error();
            auto entry = json::Value::object();
            entry["plugin"] = fx.plugin.id;
            entry["name"] = fx.plugin.name;
            entry["state"] = base64Encode(*state);
            if (fx.patch) entry["patch"] = *fx.patch;
            list.push_back(std::move(entry));
        }
        return list;
    };
    auto tracks = json::Value::array();
    for (const auto& e : app.head().tracks) {
        if (e.id == model::kMaster) continue;
        auto tr = json::Value::object();
        tr["name"] = e.value->name;
        tr["volume"] = std::pow(10.0, e.value->volumeDb / 20.0);
        tr["armed"] = e.value->record.armed;
        auto fx = chain(*e.value);
        if (!fx) return fx.error();
        tr["fx"] = *fx;
        auto notes = json::Value::array();
        for (const auto& item : app.head().items) {
            if (item.value->track != e.id) continue;
            const auto& take = *app.head().takes.find(*item.value->activeTake);
            const auto* source = app.head().midi.find(*take.source);
            if (!source) return Error("bench.midi", "expected a notation MIDI source");
            for (const auto& n : source->sequence.notes) {
                auto note = json::Value::object();
                note["start"] = double(n.start) / source->sequence.ppq;
                note["end"] = double(n.start + n.length) / source->sequence.ppq;
                note["key"] = int(n.pitch);
                note["velocity"] = int(n.velocity);
                notes.push_back(std::move(note));
            }
        }
        tr["notes"] = notes;
        tracks.push_back(std::move(tr));
    }
    out["tracks"] = tracks;
    auto master = chain(*app.head().tracks.find(model::kMaster));
    if (!master) return master.error();
    out["master"] = *master;
    return out;
}
}  // namespace supple::engine::bench
