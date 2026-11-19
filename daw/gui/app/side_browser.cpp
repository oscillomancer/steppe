// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "side_browser.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <SDL3/SDL_keycode.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <system_error>
#include <utility>

#include "fx_window.h"
#include "plugin_browser.h"
#include "supple/base/paths.h"
#include "supple/ui/canvas.h"
#include "supple/ui/list.h"
#include "supple/ui/scroll_view.h"
#include "supple/ui/text_field.h"
#include "supple/ui/theme.h"
#include "supple/ui/widgets.h"

namespace supple::gui {

namespace {

const cmd::Origin kUiOrigin{cmd::Origin::Type::Ui, "gui", {}};

struct Kind {
    const char* id;
    const char* title;
};
constexpr std::array<Kind, 4> kKinds{
    {{"plugins", "Plug-ins"}, {"chains", "Chains"}, {"files", "Files"}, {"media", "Media"}}};

// What File > Import takes, by extension, and what the row calls it.
struct FileType {
    const char* extension;
    const char* name;
};
constexpr std::array<FileType, 5> kFileTypes{
    {{".wav", "WAV"}, {".flac", "FLAC"}, {".mp3", "MP3"}, {".mid", "MIDI"}, {".midi", "MIDI"}}};

const char* fileTypeName(const std::filesystem::path& file) {
    std::string ext = file.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const FileType& type : kFileTypes)
        if (ext == type.extension) return type.name;
    return nullptr;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// The kinds side by side: the one shown is raised. A click picks one; with
// the focus, Left and Right do.
class Kinds : public ui::Node {
  public:
    Kinds(std::string id, const std::string& shown, std::function<void(const std::string&)> pick)
        : Node(std::move(id), ui::Role::Tab), shown_(shown), pick_(std::move(pick)) {
        interactive = true;
        focusable = true;
        layout.height = ui::SizeRule::fixed(24);
    }
    std::string valueText() const override {
        for (const Kind& k : kKinds)
            if (shown_ == k.id) return k.title;
        return {};
    }
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        c.fillRound(bounds(), 5, t.color("ground"));
        if (focused()) c.strokeRound(bounds(), 5, t.color("focus"));
        for (std::size_t i = 0; i < kKinds.size(); ++i) {
            const ui::Rect r = part(i);
            const bool on = shown_ == kKinds[i].id;
            if (on) c.fillRound(r.inset(2), 4, t.color("raised"));
            c.text(r, kKinds[i].title, on ? ui::Font::UiBold : ui::Font::Ui, t.type("small"),
                   on ? t.color("ink") : t.color("ink-quiet"), ui::kCenter | ui::kMiddle);
        }
    }
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.type != ui::PointerEvent::Type::Down || e.button != 1)
            return e.type != ui::PointerEvent::Type::Move;
        for (std::size_t i = 0; i < kKinds.size(); ++i)
            if (part(i).contains(e.pos)) pick_(kKinds[i].id);
        return true;
    }
    bool onKey(const ui::KeyEvent& e) override {
        if (e.key != SDLK_LEFT && e.key != SDLK_RIGHT) return false;
        std::size_t at = 0;
        for (std::size_t i = 0; i < kKinds.size(); ++i)
            if (shown_ == kKinds[i].id) at = i;
        if (e.key == SDLK_LEFT && at > 0) pick_(kKinds[at - 1].id);
        if (e.key == SDLK_RIGHT && at + 1 < kKinds.size()) pick_(kKinds[at + 1].id);
        return true;
    }

  private:
    ui::Rect part(std::size_t i) const {
        const ui::Rect b = bounds();
        const float w = b.w / static_cast<float>(kKinds.size());
        return {b.x + w * static_cast<float>(i), b.y, w, b.h};
    }
    const std::string& shown_;
    std::function<void(const std::string&)> pick_;
};

// A path on one line. When it does not fit, its start is cut, at a
// separator where there is one, so that the last folders stay readable: the
// end of a path says where you are.
class PathLabel : public ui::Node {
  public:
    explicit PathLabel(std::string id) : Node(std::move(id), ui::Role::Label) {}
    void setText(std::string text) {
        if (text == text_) return;
        text_ = std::move(text);
        invalidate();
    }
    const std::string& shown() const { return shown_; }
    std::string valueText() const override { return text_; }
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        const float size = t.type("caption");
        shown_ = fitted(c, size, bounds().w);
        c.text(bounds(), shown_, ui::Font::Mono, size, t.color("ink-quiet"));
    }

  private:
    std::string fitted(const ui::Canvas& c, float size, float width) const {
        auto fits = [&](const std::string& s) {
            return c.textWidth(s, ui::Font::Mono, size) <= width;
        };
        if (fits(text_)) return text_;
        const std::string ellipsis = "\xE2\x80\xA6";
        // From the left, the first separator from which the rest fits.
        for (std::size_t i = 1; i < text_.size(); ++i)
            if ((text_[i] == '/' || text_[i] == '\\') && fits(ellipsis + text_.substr(i)))
                return ellipsis + text_.substr(i);
        // One long name: its end, cut on a UTF-8 boundary.
        for (std::size_t i = 1; i < text_.size(); ++i) {
            if ((static_cast<unsigned char>(text_[i]) & 0xC0) == 0x80) continue;
            if (fits(ellipsis + text_.substr(i))) return ellipsis + text_.substr(i);
        }
        return ellipsis;
    }
    std::string text_, shown_;
};

// A length as the Media page says it: "0:07", "3:25", "1:02:03".
std::string lengthText(double seconds) {
    const auto total = static_cast<long long>(seconds + 0.5);
    const long long h = total / 3600, m = total / 60 % 60, sec = total % 60;
    char buf[32];
    if (h > 0)
        std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", h, m, sec);
    else
        std::snprintf(buf, sizeof buf, "%lld:%02lld", m, sec);
    return buf;
}

// Play and Stop are one button: wide enough for either, so that it and Add
// stay where they are when it changes. Add beside it is as wide.
constexpr float kPlayWidth = 52;

std::string effectsText(int count) {
    return count == 1 ? std::string("1 effect") : std::to_string(count) + " effects";
}

}  // namespace

SideBrowser::SideBrowser(ui::Root& root, engine::EngineApp& app, cmd::Dispatcher& dispatcher,
                         const UiState& ui, FxWindow& fx, PluginDrag& drag)
    : root_(root), app_(app), dispatcher_(dispatcher), ui_(ui), fx_(fx), drag_(drag) {}

std::unique_ptr<ui::Node> SideBrowser::build() {
    auto page = std::make_unique<ui::Node>("side.browser");
    page->layout.dir = ui::Layout::Dir::Column;
    auto& top = page->emplace<ui::Node>("side.browser.top");
    top.layout.dir = ui::Layout::Dir::Column;
    top.layout.height = ui::SizeRule::content();
    top.layout.padding = {12, 10, 12, 0};
    kinds_ = &top.emplace<Kinds>("side.browser.kinds", kind_,
                                 [this](const std::string& kind) { show(kind); });
    // Over the Browser itself a dragged row is at home, not refused.
    drag_.home = [this] {
        const ui::Node* browser = root_.find("side.browser");
        return browser ? browser->bounds() : ui::Rect{};
    };
    // A plugin dragged out of the list lands on the track under the pointer.
    drag_.onDrop = [this](const model::Id& track, const Status& added) {
        fx_.sideBrowser().setStatus(added ? "Added to " + model::trackLabel(app_.head(), track)
                                          : added.error().message);
    };
    auto plugins = fx_.buildSideBrowser(
        ui_, [this](const std::string& plugin, const std::string& name, ui::DragPhase phase,
                    ui::Point at) { drag_.drag(plugin, name, phase, at); });
    pluginsPage_ = plugins.get();
    page->add(std::move(plugins));
    auto chains = buildChains();
    chainsPage_ = chains.get();
    chainsPage_->visible = false;
    page->add(std::move(chains));
    auto filesPage = buildFiles();
    filesPage_ = filesPage.get();
    filesPage_->visible = false;
    page->add(std::move(filesPage));
    auto mediaPage = buildMedia();
    mediaPage_ = mediaPage.get();
    mediaPage_->visible = false;
    page->add(std::move(mediaPage));
    return page;
}

ui::Node* SideBrowser::pageOf(const std::string& kind) const {
    return kind == "plugins"  ? pluginsPage_
           : kind == "chains" ? chainsPage_
           : kind == "files"  ? filesPage_
                              : mediaPage_;
}

bool SideBrowser::importable(const std::filesystem::path& file) {
    return fileTypeName(file) != nullptr;
}

std::unique_ptr<ui::Node> SideBrowser::buildFiles() {
    auto page = std::make_unique<ui::Node>("side.browser.files");
    page->layout.dir = ui::Layout::Dir::Column;
    page->layout.padding = ui::Insets::all(12);
    page->layout.gap = 8;
    // The folder listed, and the way to the one above it.
    auto& where = page->emplace<ui::Node>("side.browser.files.where");
    where.layout.dir = ui::Layout::Dir::Row;
    where.layout.height = ui::SizeRule::fixed(24);
    where.layout.gap = 6;
    where.layout.align = ui::Layout::Align::Center;
    auto& up = where.emplace<ui::Button>("side.browser.files.up", "Up", [this] { goUp(); });
    up.quiet = true;
    up_ = &up;
    folderLabel_ =
        &where.emplace<ui::Label>("side.browser.files.folder", "", ui::Font::UiBold, "small");
    folderLabel_->layout.width = ui::SizeRule::fraction();
    fileSearch_ =
        &page->emplace<ui::TextField>("side.browser.files.search", "", "Search this folder…");
    fileSearch_->layout.height = ui::SizeRule::fixed(26);
    fileEmpty_ = &page->emplace<ui::TextBlock>("side.browser.files.empty", "", ui::Font::Ui,
                                               "small", "ink-quiet");
    fileEmpty_->layout.height = ui::SizeRule::content();
    fileEmpty_->visible = false;
    fileList_ = &page->emplace<ui::List>("side.browser.files.results");
    fileList_->rowHeight = 24;
    fileList_->paintRow = [this](ui::Canvas& c, std::size_t i, const ui::Rect& r, bool) {
        if (i >= fileShown_.size()) return;
        const ui::Theme& t = c.theme();
        const FileRow row = fileRow(i);
        const float detail = c.textWidth(row.detail, ui::Font::Ui, t.type("caption"));
        // A folder's name is bold: it is somewhere to go, not something to add.
        c.text({r.x + 8, r.y, std::max(0.0f, r.w - 28 - detail), r.h}, row.name,
               row.folder ? ui::Font::UiBold : ui::Font::Ui, t.type("small"), t.color("ink"));
        c.text({r.x, r.y, r.w - 10, r.h}, row.detail, ui::Font::Ui, t.type("caption"),
               t.color("ink-faint"), ui::kRight | ui::kMiddle);
    };
    fileSearch_->onChange = [this](const std::string&) { filterFiles(); };
    fileSearch_->onSubmit = [this](const std::string&) {
        if (const auto row = fileList_->selected()) activateFile(*row);
    };
    fileList_->onSelect = [this](std::size_t) {
        chosen();
        playFollows();
    };
    fileSearch_->onOtherKey = [this](const ui::KeyEvent& e) { return fileList_->onKey(e); };
    fileList_->onActivate = [this](std::size_t row) { activateFile(row); };
    fileList_->onDrag = [this](std::size_t row, ui::DragPhase phase, ui::Point at) {
        // Files drag, folders do not; the file is the one the drag began on.
        if (phase == ui::DragPhase::Begin) {
            if (row >= fileShown_.size() || entries_[fileShown_[row]].folder) return;
            draggedFile_ = entries_[fileShown_[row]];
        }
        if (!draggedFile_) return;
        const Entry file = *draggedFile_;
        if (phase == ui::DragPhase::End || phase == ui::DragPhase::Cancel) draggedFile_.reset();
        drag_.dragTo(file.name, phase, at, files.where, [this, file](ui::Point released) {
            if (!files.drop) return;
            stopPlay();  // it is added: no longer only heard
            const Status dropped = files.drop(file.path, released);
            setFileStatus(dropped ? "Added " + file.name : dropped.error().message);
        });
    };
    auto& footer = page->emplace<ui::Node>("side.browser.files.footer");
    footer.layout.dir = ui::Layout::Dir::Row;
    footer.layout.height = ui::SizeRule::fixed(28);
    footer.layout.align = ui::Layout::Align::Center;
    footer.layout.gap = 6;
    footer.emplace<ui::Node>("side.browser.files.footer.rest").layout.width =
        ui::SizeRule::fraction();
    filePlay_ =
        &footer.emplace<ui::Button>("side.browser.files.play", "Play", [this] { togglePlay(); });
    filePlay_->layout.width = ui::SizeRule::fixed(kPlayWidth);
    filePlay_->enabled = false;  // until an audio file is chosen
    fileAdd_ = &footer.emplace<ui::Button>("side.browser.files.add", "Add", [this] {
        if (const auto row = fileList_->selected()) activateFile(*row);
    });
    fileAdd_->layout.width = ui::SizeRule::fixed(kPlayWidth);  // two cells alike
    fileStatus_ = &page->emplace<ui::Label>("side.browser.files.status", "", ui::Font::Ui,
                                            "caption", "ink-quiet");
    fileStatus_->layout.height = ui::SizeRule::fixed(16);
    folderPath_ = &page->emplace<PathLabel>("side.browser.files.path");
    folderPath_->layout.height = ui::SizeRule::fixed(16);
    return page;
}

std::vector<std::filesystem::path> SideBrowser::roots() const {
    if (files.roots) return files.roots();
    std::vector<std::filesystem::path> out;
#if defined(_WIN32)
    // The drives there are, without touching any of them.
    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i)
        if (drives & (DWORD{1} << i))
            out.emplace_back(std::string(1, static_cast<char>('A' + i)) + ":\\");
#endif
    return out;
}

void SideBrowser::goUp() {
    if (atRoots_) return;
    if (folder_.has_parent_path() && folder_.parent_path() != folder_)
        openFolder(folder_.parent_path());
    else if (roots().size() > 1)
        openRoots();
}

void SideBrowser::openRoots() {
    entries_.clear();
    for (const std::filesystem::path& root : roots())
        entries_.push_back({root.string(), {}, root, true});
    atRoots_ = true;
    folder_.clear();
    if (fileSearch_ && !fileSearch_->text().empty()) fileSearch_->setText("");
    if (folderLabel_) {
        folderLabel_->setText("Computer");
        static_cast<PathLabel*>(folderPath_)->setText("");
        up_->enabled = false;
    }
    setFileStatus({});
    filterFiles();
}

std::string SideBrowser::shownPath() const {
    return folderPath_ ? static_cast<const PathLabel*>(folderPath_)->shown() : std::string();
}

SideBrowser::FileRow SideBrowser::fileRow(std::size_t row) const {
    if (row >= fileShown_.size()) return {};
    const Entry& entry = entries_[fileShown_[row]];
    return {entry.name, entry.folder ? std::string("folder") : entry.kind, entry.folder};
}

void SideBrowser::setFileStatus(std::string text) {
    if (fileStatus_) fileStatus_->setText(std::move(text));
    root_.damageAll();
}

bool SideBrowser::openFolder(const std::filesystem::path& folder) {
    std::error_code ec;
    std::vector<Entry> found;
    std::filesystem::directory_iterator it(
        folder, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) {
        setFileStatus("Cannot open " + folder.filename().string() + ": " + ec.message());
        return false;
    }
    for (const std::filesystem::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) break;  // what was read so far is shown
        const std::filesystem::path path = it->path();
        const std::string name = path.filename().string();
        if (name.empty() || name.front() == '.') continue;  // hidden
        std::error_code type;
        if (it->is_directory(type) && !type) {
            found.push_back({name, {}, path, true});
            continue;
        }
        const char* kind = fileTypeName(path);
        if (kind && it->is_regular_file(type) && !type) found.push_back({name, kind, path, false});
    }
    // Folders first, then files, each by name without regard to case.
    std::sort(found.begin(), found.end(), [](const Entry& a, const Entry& b) {
        if (a.folder != b.folder) return a.folder;
        const std::string x = lower(a.name), y = lower(b.name);
        return x != y ? x < y : a.name < b.name;
    });
    folder_ = folder;
    atRoots_ = false;
    entries_ = std::move(found);
    if (fileSearch_ && !fileSearch_->text().empty()) fileSearch_->setText("");
    if (folderLabel_) {
        // A root has no name of its own: its path stands for it.
        const std::string name = folder_.filename().string();
        folderLabel_->setText(name.empty() ? folder_.string() : name);
        static_cast<PathLabel*>(folderPath_)->setText(folder_.string());
        up_->enabled =
            (folder_.has_parent_path() && folder_.parent_path() != folder_) || roots().size() > 1;
    }
    setFileStatus({});
    filterFiles();
    return true;
}

void SideBrowser::filterFiles() {
    const std::string query = fileSearch_ ? lower(fileSearch_->text()) : std::string();
    fileShown_.clear();
    for (std::size_t i = 0; i < entries_.size(); ++i)
        if (query.empty() || lower(entries_[i].name).find(query) != std::string::npos)
            fileShown_.push_back(i);
    if (!fileList_) return;
    fileEmpty_->visible = fileShown_.empty();
    fileEmpty_->setText(!fileShown_.empty() ? ""
                        : entries_.empty()  ? "No folders and no audio or MIDI files here."
                                            : "Nothing here matches.");
    fileList_->setCount(fileShown_.size());
    // A row is chosen by a click, the arrow keys or a search (its first hit):
    // Add does nothing until then.
    fileList_->select(fileShown_.empty() || query.empty() ? std::nullopt
                                                          : std::optional<std::size_t>(0));
    chosen();
    playFollows();
    fileList_->invalidate();
    root_.relayout();
    root_.damageAll();
}

void SideBrowser::activateFile(std::size_t row) {
    if (row >= fileShown_.size()) return;
    const Entry entry = entries_[fileShown_[row]];
    if (entry.folder) {
        openFolder(entry.path);
        return;
    }
    if (!files.add) return;
    stopPlay();  // it is added: no longer only heard
    const Status added = files.add(entry.path);
    setFileStatus(added ? "Added " + entry.name : added.error().message);
}

std::unique_ptr<ui::Node> SideBrowser::buildChains() {
    auto page = std::make_unique<ui::Node>("side.browser.chains");
    page->layout.dir = ui::Layout::Dir::Column;
    page->layout.padding = ui::Insets::all(12);
    page->layout.gap = 8;
    chainSearch_ =
        &page->emplace<ui::TextField>("side.browser.chains.search", "", "Search chains…");
    chainSearch_->layout.height = ui::SizeRule::fixed(26);
    chainEmpty_ = &page->emplace<ui::TextBlock>("side.browser.chains.empty", "", ui::Font::Ui,
                                                "small", "ink-quiet");
    chainEmpty_->layout.height = ui::SizeRule::content();
    chainEmpty_->visible = false;
    chainList_ = &page->emplace<ui::List>("side.browser.chains.results");
    chainList_->rowHeight = 24;
    chainList_->paintRow = [this](ui::Canvas& c, std::size_t i, const ui::Rect& r, bool) {
        if (i >= shown_.size()) return;
        const ui::Theme& t = c.theme();
        const ChainRow row = chainRow(i);
        const float detail = c.textWidth(row.detail, ui::Font::Ui, t.type("caption"));
        c.text({r.x + 8, r.y, std::max(0.0f, r.w - 28 - detail), r.h}, row.name, ui::Font::Ui,
               t.type("small"), t.color("ink"));
        c.text({r.x, r.y, r.w - 10, r.h}, row.detail, ui::Font::Ui, t.type("caption"),
               t.color("ink-faint"), ui::kRight | ui::kMiddle);
    };
    chainSearch_->onChange = [this](const std::string&) { filterChains(); };
    chainSearch_->onSubmit = [this](const std::string&) {
        if (const auto row = chainList_->selected()) addChain(*row);
    };
    chainList_->onSelect = [this](std::size_t) { chosen(); };
    chainSearch_->onOtherKey = [this](const ui::KeyEvent& e) { return chainList_->onKey(e); };
    chainList_->onActivate = [this](std::size_t row) { addChain(row); };
    chainList_->onDrag = [this](std::size_t row, ui::DragPhase phase, ui::Point at) {
        // The chain is the one the drag began on, whatever the rows become.
        if (phase == ui::DragPhase::Begin) {
            if (row >= shown_.size()) return;
            dragged_ = chains_[shown_[row]];
        }
        if (!dragged_) return;
        const Chain chain = *dragged_;
        if (phase == ui::DragPhase::End || phase == ui::DragPhase::Cancel) dragged_.reset();
        drag_.drag(chain.name, phase, at, [this, chain](const model::Id& track) {
            const Status loaded = loadChain(chain, {track});
            setChainStatus(loaded ? "Loaded onto " + model::trackLabel(app_.head(), track)
                                  : loaded.error().message);
        });
    };
    auto& footer = page->emplace<ui::Node>("side.browser.chains.footer");
    footer.layout.dir = ui::Layout::Dir::Row;
    footer.layout.height = ui::SizeRule::fixed(28);
    footer.layout.align = ui::Layout::Align::Center;
    footer.emplace<ui::Node>("side.browser.chains.footer.rest").layout.width =
        ui::SizeRule::fraction();
    chainAdd_ = &footer.emplace<ui::Button>("side.browser.chains.add", "Add", [this] {
        if (const auto row = chainList_->selected()) addChain(*row);
    });
    chainStatus_ = &page->emplace<ui::Label>("side.browser.chains.status", "", ui::Font::Ui,
                                             "caption", "ink-quiet");
    chainStatus_->layout.height = ui::SizeRule::fixed(16);
    return page;
}

SideBrowser::ChainRow SideBrowser::chainRow(std::size_t row) const {
    if (row >= shown_.size()) return {};
    const Chain& chain = chains_[shown_[row]];
    return {chain.name, effectsText(chain.effects)};
}

void SideBrowser::setChainStatus(std::string text) {
    if (chainStatus_) chainStatus_->setText(std::move(text));
    root_.damageAll();
}

std::unique_ptr<ui::Node> SideBrowser::buildMedia() {
    auto page = std::make_unique<ui::Node>("side.browser.media");
    page->layout.dir = ui::Layout::Dir::Column;
    page->layout.padding = ui::Insets::all(12);
    page->layout.gap = 8;
    mediaSearch_ = &page->emplace<ui::TextField>("side.browser.media.search", "", "Search media…");
    mediaSearch_->layout.height = ui::SizeRule::fixed(26);
    mediaList_ = &page->emplace<ui::List>("side.browser.media.results");
    mediaList_->rowHeight = 24;
    mediaList_->paintRow = [this](ui::Canvas& c, std::size_t i, const ui::Rect& r, bool) {
        if (i >= mediaShown_.size()) return;
        const ui::Theme& t = c.theme();
        const MediaRow row = mediaRow(i);
        const float detail = c.textWidth(row.detail, ui::Font::Ui, t.type("caption"));
        c.text({r.x + 8, r.y, std::max(0.0f, r.w - 28 - detail), r.h}, row.name, ui::Font::Ui,
               t.type("small"), t.color("ink"));
        c.text({r.x, r.y, r.w - 10, r.h}, row.detail, ui::Font::Ui, t.type("caption"),
               t.color("ink-faint"), ui::kRight | ui::kMiddle);
    };
    mediaSearch_->onChange = [this](const std::string&) {
        filterMedia();
        playFollows();
    };
    mediaSearch_->onSubmit = [this](const std::string&) {
        if (const auto row = mediaList_->selected()) addMedia(*row);
    };
    mediaList_->onSelect = [this](std::size_t) {
        chosen();
        playFollows();
    };
    mediaSearch_->onOtherKey = [this](const ui::KeyEvent& e) { return mediaList_->onKey(e); };
    mediaList_->onActivate = [this](std::size_t row) { addMedia(row); };
    mediaList_->onDrag = [this](std::size_t row, ui::DragPhase phase, ui::Point at) {
        // It drags into the arrangement as a file does; the one the drag
        // began on, whatever the rows become.
        if (phase == ui::DragPhase::Begin) {
            if (row >= mediaShown_.size()) return;
            draggedMedia_ = media_[mediaShown_[row]];
        }
        if (!draggedMedia_) return;
        const Media media = *draggedMedia_;
        if (phase == ui::DragPhase::End || phase == ui::DragPhase::Cancel) draggedMedia_.reset();
        drag_.dragTo(media.name, phase, at, files.where, [this, media](ui::Point released) {
            if (!files.drop) return;
            stopPlay();
            const Status dropped = files.drop(media.path, released);
            setMediaStatus(dropped ? "Added " + media.name : dropped.error().message);
        });
    };
    auto& footer = page->emplace<ui::Node>("side.browser.media.footer");
    footer.layout.dir = ui::Layout::Dir::Row;
    footer.layout.height = ui::SizeRule::fixed(28);
    footer.layout.align = ui::Layout::Align::Center;
    footer.layout.gap = 6;
    footer.emplace<ui::Node>("side.browser.media.footer.rest").layout.width =
        ui::SizeRule::fraction();
    mediaPlay_ =
        &footer.emplace<ui::Button>("side.browser.media.play", "Play", [this] { togglePlay(); });
    mediaPlay_->layout.width = ui::SizeRule::fixed(kPlayWidth);
    mediaPlay_->enabled = false;
    mediaAdd_ = &footer.emplace<ui::Button>("side.browser.media.add", "Add", [this] {
        if (const auto row = mediaList_->selected()) addMedia(*row);
    });
    mediaAdd_->enabled = false;  // until a row is chosen; an empty project never lists one
    mediaAdd_->layout.width = ui::SizeRule::fixed(kPlayWidth);
    mediaStatus_ = &page->emplace<ui::Label>("side.browser.media.status", "", ui::Font::Ui,
                                             "caption", "ink-quiet");
    mediaStatus_->layout.height = ui::SizeRule::fixed(16);
    return page;
}

SideBrowser::MediaRow SideBrowser::mediaRow(std::size_t row) const {
    if (row >= mediaShown_.size()) return {};
    const Media& media = media_[mediaShown_[row]];
    return {media.name, media.detail};
}

void SideBrowser::setMediaStatus(std::string text) {
    if (mediaStatus_) mediaStatus_->setText(std::move(text));
    root_.damageAll();
}

void SideBrowser::sync() {
    if (!auditioned_.empty()) {
        // It ended, was stopped or replaced from elsewhere (an agent's
        // audition.play, the engine stopping, another project): Play again.
        const engine::AuditionStatus now = app_.auditionStatus();
        if (!now.playing || std::filesystem::path(now.path) != auditioned_) {
            playEnded({});
        } else if (now.decoding != auditionReading_) {
            auditionReading_ = now.decoding;
            setPlayStatus(
                playingText(auditionedName_, auditionedSeconds_, auditionedCut_, auditionReading_));
        }
    }
    if (!visible_ || kind_ != "media") return;
    if (app_.head().version != mediaVersion_ || app_.projectGeneration() != mediaGeneration_)
        loadMedia(true);
}

// The session's audio sources, by file name without regard to case. A list
// that comes out as it was is left alone, so an edit elsewhere in the
// project does not take the chosen row away.
// `whenChanged` (the loop's pass): an edit that left the project's sources
// and its media folder as they were has nothing new to list, and the files
// are not looked for on disk again; most edits are of that kind, a fader's
// drag makes many of them, and a look is a file system call for each source.
void SideBrowser::loadMedia(bool whenChanged) {
    const model::Session& s = app_.head();
    const bool sameProject = app_.projectGeneration() == mediaGeneration_;
    const std::filesystem::path dir = app_.mediaDir();
    mediaVersion_ = s.version;
    mediaGeneration_ = app_.projectGeneration();
    if (whenChanged && sameProject && s.media.sameStorage(mediaSeen_) && dir == mediaDir_) return;
    mediaSeen_ = s.media;
    mediaDir_ = dir;
    std::vector<Media> found;
    for (const auto& e : s.media) {
        Media media;
        media.id = e.id.str();
        media.path = std::filesystem::path(e.value->path);
        if (media.path.is_relative()) media.path = dir / media.path;
        media.name = media.path.filename().string();
        std::string format = e.value->format;
        for (char& c : format) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        std::error_code ec;
        if (!std::filesystem::exists(media.path, ec))
            media.detail = "missing";
        else if (e.value->sampleRate > 0)
            media.detail = (format.empty() ? std::string() : format + " \u00b7 ") +
                           lengthText(static_cast<double>(e.value->frames) / e.value->sampleRate);
        else
            media.detail = format;
        found.push_back(std::move(media));
    }
    std::sort(found.begin(), found.end(), [](const Media& a, const Media& b) {
        const std::string x = lower(a.name), y = lower(b.name);
        return x != y ? x < y : a.id < b.id;
    });
    if (found == media_) return;
    // The chosen row stays chosen while its source is still listed. Another
    // project's source with the same id is another file, and is not chosen.
    std::string chosenId;
    if (mediaList_ && sameProject)
        if (const auto row = mediaList_->selected(); row && *row < mediaShown_.size())
            chosenId = media_[mediaShown_[*row]].id;
    media_ = std::move(found);
    filterMedia();
    if (mediaList_ && !chosenId.empty()) {
        for (std::size_t i = 0; i < mediaShown_.size(); ++i)
            if (media_[mediaShown_[i]].id == chosenId) mediaList_->select(i);
        chosen();
    }
    playFollows();  // after the row is chosen again: a source that went stops
}

void SideBrowser::filterMedia() {
    const std::string query = mediaSearch_ ? lower(mediaSearch_->text()) : std::string();
    mediaShown_.clear();
    for (std::size_t i = 0; i < media_.size(); ++i)
        if (query.empty() || lower(media_[i].name).find(query) != std::string::npos)
            mediaShown_.push_back(i);
    if (!mediaList_) return;
    mediaList_->setCount(mediaShown_.size());
    mediaList_->select(mediaShown_.empty() || query.empty() ? std::nullopt
                                                            : std::optional<std::size_t>(0));
    chosen();
    mediaList_->invalidate();
    root_.relayout();
    root_.damageAll();
}

// As a file from the Files page: `item.add` knows the bytes and uses the
// source the project has, so nothing is copied and no second source is made.
void SideBrowser::addMedia(std::size_t row) {
    if (row >= mediaShown_.size() || !files.add) return;
    const Media media = media_[mediaShown_[row]];
    stopPlay();
    const Status added = files.add(media.path);
    setMediaStatus(added ? "Added " + media.name : added.error().message);
}

void SideBrowser::setVisible(bool visible) {
    visible_ = visible;
    if (visible)
        reload();
    else
        stopPlay();  // out of view: nothing goes on playing unseen
}

// --- Play: hearing a file before it is added (ADR-0094) ---------------------

std::string SideBrowser::playingText(const std::string& name, double seconds, bool truncated,
                                     bool reading) {
    // A long file is read before it sounds; only its start is played.
    const std::string what = truncated ? "the first " + lengthText(seconds) + " of " + name : name;
    return reading ? "Reading " + what + "\u2026" : "Playing " + what;
}

std::optional<SideBrowser::Playable> SideBrowser::playable() const {
    if (kind_ == "files" && fileList_) {
        const auto row = fileList_->selected();
        if (!row || *row >= fileShown_.size()) return std::nullopt;
        const Entry& entry = entries_[fileShown_[*row]];
        // A MIDI file has no instrument here to play it.
        if (entry.folder || entry.kind == "MIDI") return std::nullopt;
        return Playable{entry.path, entry.name};
    }
    if (kind_ == "media" && mediaList_) {
        const auto row = mediaList_->selected();
        if (!row || *row >= mediaShown_.size()) return std::nullopt;
        const Media& media = media_[mediaShown_[*row]];
        return Playable{media.path, media.name};
    }
    return std::nullopt;
}

void SideBrowser::setPlayStatus(std::string text) {
    if (kind_ == "files")
        setFileStatus(std::move(text));
    else if (kind_ == "media")
        setMediaStatus(std::move(text));
}

void SideBrowser::togglePlay() {
    if (!auditioned_.empty()) {
        stopPlay();
        return;
    }
    const auto file = playable();
    if (!file) return;
    json::Value args = json::Value::object();
    args["path"] = file->path.string();
    const auto played = dispatcher_.execute({"audition.play", args, kUiOrigin});
    if (!played) {
        setPlayStatus(played.error().message);
        return;
    }
    auditioned_ = file->path;
    auditionedName_ = file->name;
    const json::Value* seconds = played->find("seconds");
    const json::Value* cut = played->find("truncated");
    auditionedSeconds_ = seconds && seconds->isNumber() ? seconds->asDouble() : 0;
    auditionedCut_ = cut && cut->isBool() && cut->asBool();
    auditionReading_ = app_.auditionStatus().decoding;
    for (ui::Button* play : {filePlay_, mediaPlay_})
        if (play) play->setText("Stop");
    setPlayStatus(
        playingText(auditionedName_, auditionedSeconds_, auditionedCut_, auditionReading_));
}

void SideBrowser::stopPlay() {
    if (auditioned_.empty()) return;
    // Only what was played from here: another file an agent has since
    // started is not this page's to stop.
    const engine::AuditionStatus now = app_.auditionStatus();
    if (now.playing && std::filesystem::path(now.path) == auditioned_)
        (void)dispatcher_.execute({"audition.stop", json::Value::object(), kUiOrigin});
    playEnded({});
}

void SideBrowser::playFollows() {
    if (auditioned_.empty()) return;
    const auto file = playable();
    if (!file || file->path != auditioned_) stopPlay();
}

void SideBrowser::playEnded(std::string say) {
    if (auditioned_.empty()) return;
    const std::string was[] = {
        playingText(auditionedName_, auditionedSeconds_, auditionedCut_, false),
        playingText(auditionedName_, auditionedSeconds_, auditionedCut_, true)};
    auditioned_.clear();
    auditionedName_.clear();
    auditionReading_ = false;
    for (ui::Button* play : {filePlay_, mediaPlay_})
        if (play) play->setText("Play");
    // The line stops saying it plays; what was said since (Added…) stays.
    for (ui::Label* status : {fileStatus_, mediaStatus_})
        if (status && (status->valueText() == was[0] || status->valueText() == was[1]))
            status->setText(say);
    root_.damageAll();
}

void SideBrowser::auditionFailed(const json::Value& event) {
    const json::Value* path = event.find("path");
    const json::Value* message = event.find("message");
    if (auditioned_.empty() || !path || !path->isString() ||
        std::filesystem::path(path->asString()) != auditioned_)
        return;
    playEnded(message && message->isString() ? message->asString() : std::string());
}

void SideBrowser::show(const std::string& kind) {
    const bool known =
        std::any_of(kKinds.begin(), kKinds.end(), [&](const Kind& k) { return kind == k.id; });
    if (!known || kind == kind_ || !pluginsPage_) return;
    stopPlay();  // what the page leaving view was playing
    // The page leaving view gives up the keyboard.
    root_.releaseWithin(pageOf(kind_));
    kind_ = kind;
    for (const Kind& k : kKinds) pageOf(k.id)->visible = kind_ == k.id;
    if (visible_) reload();
    root_.relayout();
    root_.damageAll();
}

void SideBrowser::focusSearch() {
    if (kind_ == "plugins")
        fx_.sideBrowser().focusSearch();
    else if (ui::TextField* search = kind_ == "chains"  ? chainSearch_
                                     : kind_ == "files" ? fileSearch_
                                                        : mediaSearch_)
        root_.focus(search);
}

void SideBrowser::reload() {
    if (kind_ == "plugins") {
        fx_.sideBrowser().reload();
        return;
    }
    if (kind_ == "media") {
        loadMedia(false);  // in view again: the files are looked for too
        return;
    }
    if (kind_ == "files" && atRoots_) {
        openRoots();
        return;
    }
    if (kind_ == "files") {
        // The first time: the project's folder, or the home folder for a
        // project not saved yet. After that, wherever the user went; a
        // folder that has gone falls back the same way.
        std::error_code ec;
        if (folder_.empty() || !std::filesystem::is_directory(folder_, ec)) {
            const std::filesystem::path project = app_.projectPath();
            folder_ = !project.empty() && project.has_parent_path() ? project.parent_path()
                                                                    : paths::home();
        }
        const std::string search = fileSearch_ ? fileSearch_->text() : std::string();
        if (openFolder(folder_) && fileSearch_ && !search.empty()) {
            fileSearch_->setText(search);  // a reload keeps what was typed
            filterFiles();
        }
        return;
    }
    chains_.clear();
    auto listed = dispatcher_.execute({"fx.chain.list", json::Value::object(), kUiOrigin});
    if (!listed) {
        setChainStatus(listed.error().message);
    } else if (const json::Value* chains = listed->find("chains"); chains && chains->isArray()) {
        for (const json::Value& c : chains->asArray()) {
            Chain chain;
            if (const json::Value* v = c.find("name"); v && v->isString())
                chain.name = v->asString();
            if (const json::Value* v = c.find("path"); v && v->isString())
                chain.path = v->asString();
            if (const json::Value* v = c.find("effects"); v && v->isNumber())
                chain.effects = static_cast<int>(v->asDouble());
            if (!chain.path.empty()) chains_.push_back(std::move(chain));
        }
    }
    filterChains();
}

// Add is for the row chosen: a stray click on it must not put the list's
// first plug-in chain or file on every selected track.
void SideBrowser::chosen() {
    if (chainAdd_ && chainList_) chainAdd_->enabled = chainList_->selected().has_value();
    if (fileAdd_ && fileList_) fileAdd_->enabled = fileList_->selected().has_value();
    if (mediaAdd_ && mediaList_) mediaAdd_->enabled = mediaList_->selected().has_value();
    // Play is for an audio file: not a folder, not a MIDI file.
    if (filePlay_ && fileList_) {
        const auto row = fileList_->selected();
        const Entry* entry =
            row && *row < fileShown_.size() ? &entries_[fileShown_[*row]] : nullptr;
        filePlay_->enabled = entry && !entry->folder && entry->kind != "MIDI";
    }
    if (mediaPlay_ && mediaList_) mediaPlay_->enabled = mediaList_->selected().has_value();
    root_.damageAll();
}

void SideBrowser::filterChains() {
    const std::string query = chainSearch_ ? lower(chainSearch_->text()) : std::string();
    shown_.clear();
    for (std::size_t i = 0; i < chains_.size(); ++i)
        if (query.empty() || lower(chains_[i].name).find(query) != std::string::npos)
            shown_.push_back(i);
    if (!chainList_) return;
    // Nothing saved yet: say where chains come from, in the list's place.
    const bool none = chains_.empty();
    chainEmpty_->visible = none || shown_.empty();
    chainEmpty_->setText(none ? "No saved chains yet. Save one with Save chain… in a track's FX "
                                "window, and it is listed here."
                         : shown_.empty() ? "No chain matches."
                                          : "");
    chainList_->setCount(shown_.size());
    chainList_->select(shown_.empty() || query.empty() ? std::nullopt
                                                       : std::optional<std::size_t>(0));
    chosen();
    chainList_->invalidate();
    root_.relayout();
    root_.damageAll();
}

Status SideBrowser::loadChain(const Chain& chain, const std::vector<model::Id>& tracks) {
    auto one = [&](const model::Id& track) {
        json::Value args = json::Value::object();
        args["track"] = track.str();
        args["path"] = chain.path;
        return dispatcher_.execute({"fx.chain.load", std::move(args), kUiOrigin});
    };
    // Several tracks: all of them or none, as one undo step.
    auto result = tracks.size() == 1
                      ? one(tracks.front())
                      : dispatcher_.transact("Load chain", kUiOrigin, [&]() -> Result<json::Value> {
                            for (const model::Id& track : tracks)
                                if (auto r = one(track); !r) return r;
                            return json::Value::object();
                        });
    if (!result) return result.error();
    return {};
}

void SideBrowser::addChain(std::size_t row) {
    if (row >= shown_.size()) return;
    const Chain chain = chains_[shown_[row]];
    const model::Session& s = app_.head();
    std::vector<model::Id> tracks;
    for (const std::string& selected : ui_.selectedTracks)
        if (const auto id = model::Id::parse(selected); id && s.tracks.find(*id))
            tracks.push_back(*id);
    if (tracks.empty()) {
        setChainStatus("Select a track first");
        return;
    }
    const Status loaded = loadChain(chain, tracks);
    setChainStatus(!loaded ? loaded.error().message
                   : tracks.size() == 1
                       ? "Loaded onto " + model::trackLabel(app_.head(), tracks.front())
                       : "Loaded onto " + std::to_string(tracks.size()) + " tracks");
}

}  // namespace supple::gui
