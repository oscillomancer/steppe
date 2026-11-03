// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The File actions (new, open, save, save as, project templates, import
// audio) with the system's file dialogs (SDL3: the desktop portal or a native
// dialog, asynchronous — the UI thread never waits on them) and a confirmation
// before unsaved changes are discarded. Every change goes through the bus.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "app_actions.h"
#include "supple/ui/root.h"

struct SDL_Window;

namespace supple::gui {

// Native callbacks may race shutdown; posted completions run on the UI thread.
class DialogLifetime {
  public:
    using Paths = std::vector<std::filesystem::path>;
    using Post = std::function<void(std::function<void()>)>;
    DialogLifetime() = default;
    DialogLifetime(const DialogLifetime&) = delete;
    DialogLifetime& operator=(const DialogLifetime&) = delete;
    ~DialogLifetime();
    std::function<void(Paths)> guard(Post post, std::function<void(Paths)> done) const;

  private:
    struct State {
        std::mutex mutex;
        bool active = true;
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
};

// Where projects and renders go unless the user picks another folder:
// ~/Documents/<slug>-media (empty without HOME).
std::filesystem::path mediaHome();

// Where Save as puts a project: a folder of its own named after it, with its
// own media folder — <chosen folder>/<name>/<name>.json — unless the chosen
// folder already is <name>; from inside `current`'s folder, beside it.
std::filesystem::path projectFolderPath(std::filesystem::path chosen,
                                        const std::filesystem::path& current);

class FileActions {
  public:
    using Post = std::function<void(std::function<void()>)>;
    // Injectable native Save As boundary; completions still use the UI/lifetime guard.
    using SaveDialog =
        std::function<void(const std::string&, std::function<void(DialogLifetime::Paths)>)>;
    FileActions(ui::Root& root, AppActions& actions, cmd::Dispatcher& dispatcher,
                engine::EngineApp& app, UiState& ui, SDL_Window* window, Post post,
                SaveDialog saveDialog = {});

    // Menu/shortcut and native window-close requests share the registered action.
    void registerQuit(std::function<void()> quit, bool prompt = true);
    bool dirty() const;  // changes since the last save/open/new
    // Finalizes recording through Stop, then runs `then` if nothing would be
    // lost; otherwise asks Save / Don't save / Cancel. A Stop error aborts.
    // Hidden/finite test runs suppress the prompt, but still finalize recording.
    void confirmDiscard(const std::string& what, std::function<void()> then, bool prompt = true);
    // Native results belong to the project that opened the dialog. For Open,
    // changed revisions require a fresh discard decision before delivery.
    std::function<void(DialogLifetime::Paths)> guardDialog(
        std::function<void(DialogLifetime::Paths)> done, bool opening = false);
    // For tests: what a dialog would have returned.
    void openPath(const std::filesystem::path& path);
    void saveAs(const std::filesystem::path& path);  // in projectFolderPath()
    void import(const std::vector<std::filesystem::path>& files);
    // Templates — of the project (File > Save template… / Load template…) or
    // of one strip (Track > …, the first selected track): a name prompt, and a
    // list where a click starts a new project from a template, or adds a
    // track template's strip and selects it, and a right click offers to
    // delete it.
    enum class Template { Project, Track };
    void openSaveTemplate(Template kind = Template::Project);
    void openTemplateList(Template kind = Template::Project);
    void saveTemplate(const std::string& name,
                      Template kind = Template::Project);  // the prompt's Save
    void loadTemplate(const std::string& name);            // after confirmDiscard
    void loadTrackTemplate(const std::string& name);
    // The system's folder dialog, starting at `start`; `done` runs on the UI
    // thread with the chosen folder.
    void chooseFolder(const std::filesystem::path& start,
                      std::function<void(std::filesystem::path)> done);
    // The system's open dialog for a PNG or JPEG (a render cover).
    void chooseImage(const std::filesystem::path& start,
                     std::function<void(std::filesystem::path)> done);
    // A save's (or template's) warnings, if it has any, in a popup: a plug-in
    // whose state could not be read keeps its earlier state.
    void showSaveWarnings(const std::string& command, const json::Value& result);

  private:
    struct DiscardRequest;
    bool currentProject(std::uint64_t generation);
    void showDiscard(const std::string& what, std::function<void()> then);
    bool revalidateDiscard(const std::shared_ptr<DiscardRequest>& request);
    void chooseSave(std::function<void(DialogLifetime::Paths)> done);
    std::string dialogStart() const;  // the project's file, else mediaHome()
    Result<json::Value> exec(const std::string& command, json::Value args);
    void markSaved();
    void showError(const std::string& message);
    ui::Root& root_;
    AppActions& actions_;
    cmd::Dispatcher& dispatcher_;
    engine::EngineApp& app_;
    UiState& ui_;
    SDL_Window* window_;
    Post post_;
    SaveDialog saveDialog_;
    DialogLifetime dialogs_;
    std::uint64_t savedVersion_ = 0;
    std::uint64_t projectGeneration_ = ~std::uint64_t{0};
    std::string savedBranch_;
    std::string replaceTemplate_;  // a name that exists: Save again replaces it
};

}  // namespace supple::gui
