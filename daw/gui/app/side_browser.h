// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The side panel's Browser tab (docs/specs/gui.md §4.14): a row of kinds
// over one page per kind. Plug-ins is the plugin browser built as a panel's
// content; Chains lists the saved FX chain presets; Files lists a folder's
// audio and MIDI files. What is chosen goes on the selected tracks, or
// where it is dragged, through commands that exist (`fx.add`,
// `fx.chain.load`, `item.add`).
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app_actions.h"
#include "plugin_drag.h"
#include "supple/cmd/bus.h"
#include "supple/engine/app.h"
#include "supple/ui/root.h"

namespace supple::ui {
class Label;
class List;
class TextBlock;
class TextField;
}  // namespace supple::ui

namespace supple::gui {

class FxWindow;

class SideBrowser {
  public:
    // `ui` (the selection) and the others outlive the browser.
    SideBrowser(ui::Root& root, engine::EngineApp& app, cmd::Dispatcher& dispatcher,
                const UiState& ui, FxWindow& fx, PluginDrag& drag);
    // What the Files page does with a file; set before build(). The app
    // gives File > Import's own path and the arrange view's file drop.
    struct FileHooks {
        // Add: onto the selected track or a new one, at the playhead.
        std::function<Status(const std::filesystem::path&)> add;
        // A drag: where it may land (the part of the window to outline),
        // and the drop at the release point.
        PluginDrag::Place where;
        std::function<Status(const std::filesystem::path&, ui::Point)> drop;
        // The file system's roots, for a test. Unset: the drives on
        // Windows, and none elsewhere (one root, nothing to choose).
        std::function<std::vector<std::filesystem::path>()> roots;
    };
    FileHooks files;
    std::unique_ptr<ui::Node> build();  // once
    // The tab came into view or left it: in view, the kind shown reads its
    // list again (the scan cache and favourites, the chains folder).
    void setVisible(bool visible);
    // The kinds, in the row's order: "plugins", "chains", "files". Unknown:
    // ignored.
    void show(const std::string& kind);
    const std::string& kind() const { return kind_; }
    void focusSearch();  // the shown kind's search field; typing searches

    // The chains as listed (after the search), for tests: a row's name and
    // what is at its right ("3 effects").
    struct ChainRow {
        std::string name, detail;
    };
    std::size_t chainCount() const { return shown_.size(); }
    ChainRow chainRow(std::size_t row) const;

    // The Files page: the folder listed, and its rows after the search
    // (folders first, then the files that can be imported), for tests.
    // openFolder lists another; false and a status line when it cannot.
    static bool importable(const std::filesystem::path& file);  // by extension
    const std::filesystem::path& folder() const { return folder_; }
    bool openFolder(const std::filesystem::path& folder);
    // The roots as a list of folders (Up from a root where there are
    // several: the drives on Windows). folder() is then empty.
    void openRoots();
    struct FileRow {
        std::string name, detail;  // detail: "folder", "WAV", "MIDI"…
        bool folder = false;
    };
    std::size_t fileCount() const { return fileShown_.size(); }
    FileRow fileRow(std::size_t row) const;

  private:
    struct Chain {
        std::string name, path;
        int effects = 0;
    };
    struct Entry {
        std::string name, kind;
        std::filesystem::path path;
        bool folder = false;
    };
    std::unique_ptr<ui::Node> buildChains();
    std::unique_ptr<ui::Node> buildFiles();
    ui::Node* pageOf(const std::string& kind) const;
    std::vector<std::filesystem::path> roots() const;
    void goUp();
    void filterFiles();
    void activateFile(std::size_t row);  // open a folder, add a file
    void setFileStatus(std::string text);
    void reload();        // the kind in view
    void filterChains();  // by the search
    void addChain(std::size_t row);
    Status loadChain(const Chain& chain, const std::vector<model::Id>& tracks);
    void setChainStatus(std::string text);

    ui::Root& root_;
    engine::EngineApp& app_;
    cmd::Dispatcher& dispatcher_;
    const UiState& ui_;
    FxWindow& fx_;
    PluginDrag& drag_;
    std::string kind_ = "plugins";
    bool visible_ = false;
    ui::Node* kinds_ = nullptr;
    ui::Node* pluginsPage_ = nullptr;
    ui::Node* chainsPage_ = nullptr;
    ui::TextField* chainSearch_ = nullptr;
    ui::List* chainList_ = nullptr;
    ui::Label* chainStatus_ = nullptr;
    ui::TextBlock* chainEmpty_ = nullptr;
    std::vector<Chain> chains_;       // as fx.chain.list gave them
    std::vector<std::size_t> shown_;  // indices into chains_, after the search
    std::optional<Chain> dragged_;    // the chain in a drag
    ui::Node* filesPage_ = nullptr;
    ui::TextField* fileSearch_ = nullptr;
    ui::List* fileList_ = nullptr;
    ui::Label* fileStatus_ = nullptr;
    ui::Label* folderLabel_ = nullptr;
    ui::Label* folderPath_ = nullptr;
    ui::TextBlock* fileEmpty_ = nullptr;
    ui::Node* up_ = nullptr;
    std::filesystem::path folder_;        // the folder listed; empty until first shown
    std::vector<Entry> entries_;          // its folders, then its files
    std::vector<std::size_t> fileShown_;  // indices into entries_, after the search
    std::optional<Entry> draggedFile_;    // the file in a drag
    bool atRoots_ = false;                // the roots are listed, not a folder
};

}  // namespace supple::gui
