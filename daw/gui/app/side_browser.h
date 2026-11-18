// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The side panel's Browser tab (docs/specs/gui.md §4.14): a row of kinds
// over one page per kind. Plug-ins is the plugin browser built as a panel's
// content; Chains lists the saved FX chain presets; Files lists a folder's
// audio and MIDI files; Media lists the audio the project already holds.
// What is chosen goes on the selected tracks, or where it is dragged,
// through commands that exist (`fx.add`, `fx.chain.load`, `item.add`).
// An audio file of the last two is heard first with Play (`audition.play`).
#pragma once

#include <cstdint>
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
class Button;
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
    // The kinds, in the row's order: "plugins", "chains", "files", "media".
    // Unknown: ignored.
    void show(const std::string& kind);
    const std::string& kind() const { return kind_; }
    void focusSearch();  // the shown kind's search field; typing searches
    // Once per pass of the loop: the Media page, while in view, lists the
    // project's media again when the project has changed; and a file played
    // from here that has ended gives its button back to Play.
    void sync();

    // Play, beside Add on the Files and Media pages: the chosen audio file
    // is heard on outputs 1/2 before it is added (`audition.play`,
    // ADR-0094); the button reads Stop meanwhile. The file played from
    // here, empty when none is.
    const std::filesystem::path& auditioned() const { return auditioned_; }
    // The engine's `audition.error` event: a file that could not be read
    // after all. Said on the page when it is the one played from here.
    void auditionFailed(const json::Value& event);
    // What the status line says while a file plays, or is still being read.
    static std::string playingText(const std::string& name, double seconds, bool truncated,
                                   bool reading);

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
    // The folder's path as its line last drew it: whole, or with its start
    // cut ("…/Loops/Drums") so that the last folders stay readable.
    std::string shownPath() const;

    // The Media page: the project's audio sources (the session's `media`)
    // after the search, by file name, for tests. `detail` is the format and
    // the length ("WAV · 0:12"), or "missing" for a file that is not there.
    struct MediaRow {
        std::string name, detail;
    };
    std::size_t mediaCount() const { return mediaShown_.size(); }
    MediaRow mediaRow(std::size_t row) const;

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
    struct Media {
        std::string id, name, detail;  // the source's id, its file's name
        std::filesystem::path path;    // absolute
        bool operator==(const Media&) const = default;
    };
    std::unique_ptr<ui::Node> buildChains();
    std::unique_ptr<ui::Node> buildFiles();
    std::unique_ptr<ui::Node> buildMedia();
    // From the session. `whenChanged`: only when its sources or its media
    // folder are not those the list was last read from.
    void loadMedia(bool whenChanged);
    void filterMedia();  // by the search
    void addMedia(std::size_t row);
    void setMediaStatus(std::string text);
    // The chosen row of the page shown, when it is an audio file.
    struct Playable {
        std::filesystem::path path;
        std::string name;
    };
    std::optional<Playable> playable() const;
    void togglePlay();                // the Play button of the page shown
    void stopPlay();                  // what was played from here, if it still plays
    void playFollows();               // the chosen row is another now: it stops
    void playEnded(std::string say);  // the buttons read Play again
    void setPlayStatus(std::string text);
    ui::Node* pageOf(const std::string& kind) const;
    std::vector<std::filesystem::path> roots() const;
    void goUp();
    void filterFiles();
    void activateFile(std::size_t row);  // open a folder, add a file
    void setFileStatus(std::string text);
    void reload();        // the kind in view
    void filterChains();  // by the search
    void chosen();        // a list's selection changed: its Add follows
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
    ui::Button* chainAdd_ = nullptr;
    ui::Button* fileAdd_ = nullptr;
    ui::Button* filePlay_ = nullptr;
    ui::Button* mediaPlay_ = nullptr;
    // The file played from here, its name, and what the engine said of it.
    std::filesystem::path auditioned_;
    std::string auditionedName_;
    double auditionedSeconds_ = 0;
    bool auditionedCut_ = false, auditionReading_ = false;
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
    ui::Node* folderPath_ = nullptr;
    ui::TextBlock* fileEmpty_ = nullptr;
    ui::Node* up_ = nullptr;
    std::filesystem::path folder_;        // the folder listed; empty until first shown
    std::vector<Entry> entries_;          // its folders, then its files
    std::vector<std::size_t> fileShown_;  // indices into entries_, after the search
    std::optional<Entry> draggedFile_;    // the file in a drag
    bool atRoots_ = false;                // the roots are listed, not a folder
    ui::Node* mediaPage_ = nullptr;
    ui::TextField* mediaSearch_ = nullptr;
    ui::List* mediaList_ = nullptr;
    ui::Button* mediaAdd_ = nullptr;
    ui::Label* mediaStatus_ = nullptr;
    std::vector<Media> media_;             // the project's audio sources, by name
    std::vector<std::size_t> mediaShown_;  // indices into media_, after the search
    std::optional<Media> draggedMedia_;    // the one in a drag
    // The session the list was read from; ~0: not read yet.
    std::uint64_t mediaVersion_ = ~std::uint64_t{0}, mediaGeneration_ = ~std::uint64_t{0};
    // Its sources (they share storage with the session's until an edit
    // changes them) and the folder their relative paths were resolved in.
    model::IdMap<model::MediaSource> mediaSeen_;
    std::filesystem::path mediaDir_;
};

}  // namespace supple::gui
