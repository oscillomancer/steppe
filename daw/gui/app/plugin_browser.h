// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The plugin browser (docs/specs/gui.md §4.5, §1.2): a searchable list over the
// scan cache with stock plugins and favourites first; Enter adds the selected
// plugin to the chain. Favourites are user config, never project state.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "supple/engine/app.h"
#include "supple/ui/list.h"
#include "supple/ui/node.h"

namespace supple::ui {
class Button;
class Root;
class TextField;
class Label;
}  // namespace supple::ui

namespace supple::gui {

// $XDG_CONFIG_HOME/<slug>/favourites.json — a supple.favourites/1.0 document
// {"favourites": ["com.suppleaudio.yarrow", …]}. Order is the user's, not sorted.
struct Favourites {
    std::vector<std::string> ids;  // plugin ids
    static std::filesystem::path defaultFile(std::string_view slug);
    static Result<Favourites> load(const std::filesystem::path& file);  // absent: empty
    Status save(const std::filesystem::path& file) const;
    bool has(std::string_view id) const;
    bool toggle(std::string_view id);  // true when it is now a favourite
};

class PluginBrowser {
  public:
    PluginBrowser(engine::EngineApp&, cmd::Dispatcher& dispatcher, ui::Root& root,
                  std::filesystem::path favouritesFile);
    // Opens the browser (titled `title`, default "Add plugin"); `onAdd` runs
    // with a plugin id on Enter or a double click. Its Close button closes it.
    void open(std::function<void(const std::string&)> onAdd, std::string title = {});
    void close();
    bool isOpen() const { return page_ != nullptr; }
    // The browser as part of another panel (the side panel's Browser tab,
    // gui.md §4.14): the same search, list, favourites and rescan, sized by
    // its parent, with no title or Close, and node ids that begin with
    // `prefix` so that a popup browser may be open beside it. `onAdd` runs
    // with a plugin id on Enter or a double click, and the browser stays.
    // Built once; reload() tells it when it comes into view.
    std::unique_ptr<ui::Node> buildEmbedded(std::string prefix,
                                            std::function<void(const std::string&)> onAdd);
    void reload();       // re-reads the favourites file and the scan cache
    void focusSearch();  // typing searches
    // A row dragged out of the list (ui::List::onDrag): the plugin's id and
    // name, the phase and the pointer. Set before the browser is built; the
    // owner draws the feedback and takes the drop.
    std::function<void(const std::string& id, const std::string& name, ui::DragPhase phase,
                       ui::Point at)>
        onDrag;

    // Frame pass: runs a rescan that was requested last frame (never inside the
    // click that asked for it, so "Scanning…" paints first).
    void tick();
    void refresh();  // re-read the scan cache and rebuild the rows
    void setStatus(std::string message);

    // State the panels and tests read back.
    const std::vector<std::string>& shown() const { return shown_; }
    const Favourites& favourites() const { return favourites_; }
    const std::filesystem::path& favouritesFile() const { return favouritesFile_; }
    // The action the browser adds through (set by open()).
    const std::function<void(const std::string&)>& onAdd() const { return onAdd_; }
    // The composed text of a row — exactly what paintRow draws, so tests can
    // read a row without pixels. `name` follows a stock plugin's with what it
    // is ("Ghyll (reverb)"); `vendor` is "stock", the vendor, or "shadowed"
    // for a copy an earlier path entry wins over; `format` is "VST3" for one,
    // else empty (the side panel's rows show it, having no id column).
    struct RowText {
        std::string name, vendor, id, format;
    };
    RowText rowText(std::size_t row) const;
    std::size_t rowCount() const { return rows_.size(); }
    // The selected plugin's own file, and the shadowed note when it is one.
    const std::string& detail() const { return detail_; }

  private:
    struct Entry {
        std::string id, name, vendor, format, path;
        std::string kind;  // a stock plugin's: "reverb", from its CLAP features; else empty
        bool instrument = false, semantic = false, shadowed = false;
    };
    struct Row {
        const Entry* entry;
        bool favourite;
    };
    std::unique_ptr<ui::Node> buildPage();
    void sortAndFilter();
    void activate(std::size_t row);
    void runScan();
    void updateDetail();

    cmd::Dispatcher& dispatcher_;
    ui::Root& root_;
    std::filesystem::path favouritesFile_;
    Favourites favourites_;
    std::vector<Entry> all_;
    std::vector<Row> rows_;           // display order, painted
    std::vector<std::string> shown_;  // ids, in display order (tests)
    std::function<void(const std::string&)> onAdd_;

    ui::Node* page_ = nullptr;
    ui::TextField* search_ = nullptr;
    ui::List* list_ = nullptr;
    ui::Button* add_ = nullptr;  // in the panel: enabled once a row is chosen
    void activateChosen();       // Add and Enter: the chosen row (the popup's first, if none)
    ui::Label* status_ = nullptr;
    ui::Label* detailLabel_ = nullptr;
    ui::Label* title_ = nullptr;
    std::string titleText_ = "Add plugin";
    std::pair<std::string, std::string> dragged_;  // the plugin in a drag: id, name
    std::string prefix_ = "browser";               // of its node ids
    bool embedded_ = false;
    std::string detail_;
    std::string scanJob_;  // the background scan being followed
};

}  // namespace supple::gui
