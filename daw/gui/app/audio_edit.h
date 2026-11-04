// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <cstdio>
#include "supple/engine/app.h"
#include "supple/model/schemas.h"

namespace supple::gui {
// UI continuations/status for detection/quantize and background item jobs
// (consolidation, mix to new track).
class AudioEdit {
  public:
    AudioEdit(engine::EngineApp& app, cmd::Dispatcher& dispatcher)
        : app_(app), dispatcher_(dispatcher) {}
    void run(const std::vector<model::Id>& selected, const std::string& operation, double grid) {
        if (pending_ || background_) {
            report("An audio edit is already running");
            return;
        }
        const auto s = app_.head();
        auto args = json::Value::object(), detect = json::Value::object();
        args["items"] = json::Value::array();
        detect["takes"] = json::Value::array();
        bool missing = false;
        for (auto id : selected) {
            const auto* it = s.items.find(id);
            const auto* t = it && it->activeTake ? s.takes.find(*it->activeTake) : nullptr;
            const auto* md = t && t->source ? s.media.find(*t->source) : nullptr;
            if (!md) continue;
            args["items"].push_back(id.str());
            detect["takes"].push_back(it->activeTake->str());
            missing = missing || !t->transients || t->transients->sourceHash != md->hash;
        }
        if (!args["items"].size()) {
            report("Select audio items first");
            return;
        }
        args["grid"] = grid;
        args["mode"] = operation;
        args["strength"] = operation == "slice" ? 0. : 1.;
        if (operation == "reset") {
            args.asObject().erase("grid");
            args.asObject().erase("mode");
            args.asObject().erase("strength");
            execute("audio.warp.reset", args);
            return;
        }
        if (operation == "detect" || missing) {
            auto job = dispatcher_.execute({"take.hits.detect", detect, origin_});
            if (!job) {
                report(job.error().toString());
                return;
            }
            pending_ =
                Pending{job->find("job")->asString(), operation == "detect" ? json::Value() : args,
                        s, app_.projectGeneration(), app_.versions().branch()};
            report("Detecting audio hits…");
        } else
            execute("audio.quantize", args);
    }
    void consolidate(const std::vector<model::Id>& selected) {
        start("item.consolidate", selected,
              {"", "Consolidating items…", "Items consolidated", "Consolidation cancelled"});
    }
    // Item: Mix to new track (item.mix-to-track): the result lands muted.
    void mixToTrack(const std::vector<model::Id>& selected) {
        if (selected.empty()) return report("Mix to new track: select items first");
        start("item.mix-to-track", selected,
              {"", "Mixing items to a new track…", "Items mixed to a new muted track",
               "Mix cancelled"});
    }
    void poll() {
        if (background_) {
            auto job = dispatcher_.jobs().status(background_->job);
            if (!job) {
                report(job.error().toString());
                background_.reset();
            } else if (job->find("status")->asString() != "running") {
                if (job->find("status")->asString() == "done")
                    report(background_->done);
                else if (const auto* error = job->find("error"))
                    report(json::writeCanonical(*error));
                else
                    report(background_->cancelled);
                background_.reset();
            }
        }
        if (!pending_) return;
        auto args = json::Value::object();
        args["job"] = pending_->job;
        auto job = dispatcher_.execute({"job.get", args, origin_});
        if (!job) {
            report(job.error().toString());
            pending_.reset();
            return;
        }
        const auto* state = job->find("status");
        if (!state || state->asString() == "running") return;
        auto pending = std::move(*pending_);
        pending_.reset();
        if (state->asString() != "done") {
            if (const auto* error = job->find("error"))
                report(json::writeCanonical(*error));
            else
                report("Audio hit detection cancelled");
            return;
        }
        if (pending.args.isNull()) {
            report("Audio hits detected");
            return;
        }
        if (pending.generation != app_.projectGeneration() ||
            pending.branch != app_.versions().branch()) {
            report("Audio selection changed; run the edit again");
            return;
        }
        const auto current = app_.head();
        for (const auto& name : pending.args["items"].asArray()) {
            const auto id = *model::Id::parse(name.asString());
            const auto* now = current.items.find(id);
            const auto* before = pending.session.items.find(id);
            if (!now || !before || model::toJson(*now) != model::toJson(*before)) {
                report("Audio selection changed; run the edit again");
                return;
            }
            auto a = *current.takes.find(*now->activeTake),
                 b = *pending.session.takes.find(*before->activeTake);
            a.transients.reset();
            b.transients.reset();
            if (model::toJson(a) != model::toJson(b)) {
                report("Audio take changed; run the edit again");
                return;
            }
        }
        execute("audio.quantize", pending.args);
    }
    std::function<void(const std::string&)> onStatus;

  private:
    // A background item job (consolidate, mix to new track) and its messages.
    struct Background {
        std::string job, running, done, cancelled;
    };
    void start(const char* command, const std::vector<model::Id>& selected, Background messages) {
        if (pending_ || background_) {
            report("An audio edit is already running");
            return;
        }
        auto args = json::Value::object();
        args["items"] = json::Value::array();
        for (auto id : selected) args["items"].push_back(id.str());
        auto job = dispatcher_.execute({command, args, origin_});
        if (!job) {
            report(job.error().toString());
            return;
        }
        messages.job = job->find("job")->asString();
        report(messages.running);
        background_ = std::move(messages);
    }
    void report(const std::string& message) {
        if (onStatus) onStatus(message);
    }
    void execute(const char* command, const json::Value& args) {
        auto result = dispatcher_.execute({command, args, origin_});
        report(result ? "Audio edit complete" : result.error().toString());
    }
    struct Pending {
        std::string job;
        json::Value args;
        model::Session session;
        std::uint64_t generation;
        std::string branch;
    };
    engine::EngineApp& app_;
    cmd::Dispatcher& dispatcher_;
    const cmd::Origin origin_{cmd::Origin::Type::Ui, "gui", {}};
    std::optional<Pending> pending_;
    std::optional<Background> background_;
};
}  // namespace supple::gui
