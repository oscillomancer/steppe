// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <atomic>
#include <memory>
#include <thread>
#include "crash_viewer.h"
#include "file_actions.h"
#include "help_support.h"

namespace supple::gui {
// Help is app-local UI, like Preferences; these actions aren't engine commands.
class HelpActions {
  public:
    using FolderPicker = std::function<void(std::function<void(std::filesystem::path)>,
                                            std::function<void(std::string)>)>;
    HelpActions(ui::Root& root, AppActions& actions, SDL_Window* window, DialogLifetime::Post post,
                std::filesystem::path executable, FolderPicker picker = {},
                std::filesystem::path diagnostics = {});
    ~HelpActions();
    void download(bool link);
    void mcp();
    void agents();
    // Help > Crash reports…: reads `diagnostics`, the folder the app keeps its reports in.
    CrashViewer& crashReports() { return crash_; }

  private:
    struct Lifetime {
        std::atomic<bool> alive{true}, cancelled{false};
    };
    using Work = std::function<std::string()>;
    std::unique_ptr<ui::Node> panel(const std::string& title);
    void notice(const std::string& title, const std::string& message, bool replace = false);
    bool translocationWarning(const std::string& title);
    void status(const std::string& message);
    void refreshButtons();
    void copy(const std::string& text);
    void background(Work work);
    void chooseFolder();
    void chosen(const std::filesystem::path& parent);
    void install(bool replace);
    void select(help::Harness harness);
    void connect();
    ui::Root& root_;
    SDL_Window* window_;
    DialogLifetime::Post post_;
    help::Executables executables_;
    FolderPicker picker_;
    bool translocated_ = false;
    std::shared_ptr<Lifetime> lifetime_ = std::make_shared<Lifetime>();
    DialogLifetime dialogs_;
    std::thread worker_;
    bool busy_ = false, link_ = false;
    unsigned generation_ = 0;
    help::Harness harness_ = help::Harness::Claude;
    std::filesystem::path folder_;
    CrashViewer crash_;
};
}  // namespace supple::gui
