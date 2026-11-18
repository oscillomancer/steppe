// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Safe-load (gui.md, ADR-0083 topic 7d): before the app loads a project, a recovery or a template,
// it asks `project.plugins` which plug-ins the work names, and for each one a recent crash report
// points at (and that still loads normally this run) it asks the person how to load it this time:
// in a separate process, not at all, or normally. The engine never prompts; only the app's own
// File > Open, recovery and template actions ask, and closing the question cancels the open.
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "app_actions.h"
#include "preferences.h"
#include "supple/base/result.h"
#include "supple/diagnostics/reports.h"
#include "supple/json/value.h"
#include "supple/ui/root.h"

namespace supple::gui {

// What the question is about.
struct LoadQuestion {
    std::string format, id, name, vendor, version;
    std::string buildId;      // the report's build; empty when it recorded none
    std::string build;        // `project.plugins`' check: "same", "version", "unreadable"
    bool stock = false;       // it comes with the app: disable only, a fault in the app
    bool bridgeable = false;  // a separate process is possible for it
    std::vector<std::string> incidents;  // the reports it settles when it is loaded normally
    // Whether the choice can be remembered: only for an exact build.
    bool rememberable() const { return !buildId.empty() && build == "same"; }
};

enum class LoadChoice { Bridge, Disable, Normal };

class SafeLoad {
  public:
    SafeLoad(ui::Root& root, cmd::Dispatcher& dispatcher, std::filesystem::path diagnostics);

    // The remembered choices, owned by the preferences.
    std::function<std::vector<RememberedLoad>()> remembered;
    std::function<Status(std::vector<RememberedLoad>)> setRemembered;
    // A line for the status bar ("Canary Verb: loaded in a separate process (remembered)…").
    std::function<void(const std::string&)> onStatus;
    // The engine's side, replaceable by tests: `project.plugins` and `plugin.override`.
    std::function<Result<json::Value>(const json::Value& args)> plugins;
    std::function<Status(const std::string& id, const std::string& mode)> override_;
    // The reports' suspects (their build ids) and settling a report: `diagnostics::suspects` and
    // `crash.settle` by default.
    std::function<std::vector<diagnostics::Suspect>()> reported;
    std::function<Status(const std::string& incident)> settle;
    // Now, in ms since the epoch, for a remembered choice's time.
    std::function<std::int64_t()> now;

    // Runs `project.plugins {args}` and asks about every suspect plug-in still on the normal load,
    // then calls `proceed` with the check's result (its `hash` and `autosave` are what the load
    // passes on). A check that cannot be made proceeds with an empty object: the load reports its
    // own error. Closing a question without choosing cancels: `proceed` never runs and nothing
    // was changed. The answers act together once all are given: the overrides (all or none),
    // the settling of what was answered "normally", then `proceed`, which returns whether the
    // load happened; when it did not, the overrides are set back and nothing is saved.
    void check(json::Value args, std::function<bool(const json::Value& found)> proceed);

    // "Enable" on a plug-in this run has disabled: loads it normally, as the question's third row
    // does, and settles the reports its suspicion rests on. An error is `plugin.override`'s.
    Status enable(const std::string& pluginId, const std::string& format = {});

    // Whether a question is open.
    bool asking() const { return asking_; }

  private:
    struct Run;
    void ask(std::shared_ptr<Run> run);
    void finish(std::shared_ptr<Run> run);
    void status(const std::string& text);
    ui::Root& root_;
    cmd::Dispatcher& dispatcher_;
    std::filesystem::path diagnostics_;
    bool asking_ = false;
};

// The question itself, a popup: `done` gets the choice and whether to remember it; closing it any
// other way (Cancel, Escape) calls `cancelled`.
void openSafeLoadDialog(ui::Root& root, const LoadQuestion& question,
                        std::function<void(LoadChoice, bool remember)> done,
                        std::function<void()> cancelled);

}  // namespace supple::gui
