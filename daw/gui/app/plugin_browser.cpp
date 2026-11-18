// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "plugin_browser.h"
#include <supple/base/paths.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "supple/base/files.h"
#include "supple/json/parse.h"
#include "supple/json/write.h"
#include "supple/product.h"
#include "supple/ui/list.h"
#include "supple/ui/root.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

const cmd::Origin kUiOrigin{cmd::Origin::Type::Ui, "gui", {}};

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool contains(std::string_view haystack, std::string_view needle) {
    return lower(haystack).find(lower(needle)) != std::string::npos;
}

const char* kFavMark = "*";  // ASCII: Inter has no visible U+2605

// What a stock plugin is, shown after its name, since its name does not say:
// the first CLAP feature after the main category ("reverb"), in the words a
// mixer uses.
std::string stockKind(const std::string& vendor, const std::vector<std::string>& features) {
    if (lower(vendor) != lower(product::kVendor) || features.size() < 2) return {};
    if (features[1] == "equalizer") return "eq";
    if (features[1] == "synthesizer") return "synth";
    std::string kind = features[1];
    std::replace(kind.begin(), kind.end(), '-', ' ');
    return kind;
}

}  // namespace

// --- Favourites -----------------------------------------------------------------

std::filesystem::path Favourites::defaultFile(std::string_view slug) {
    if (const auto root = paths::config(); !root.empty()) return root / slug / "favourites.json";
    return {};
}

Result<Favourites> Favourites::load(const std::filesystem::path& file) {
    Favourites out;
    if (file.empty()) return out;
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) && !ec) return out;
    auto bytes = readFile(file);
    if (!bytes) return std::move(bytes).error();
    auto doc =
        json::parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    if (!doc) return std::move(doc).error();
    const json::Value* schema = doc->isObject() ? doc->find("schema") : nullptr;
    if (!schema || !schema->isString() || schema->asString() != "supple.favourites/1.0")
        return Error("favourites.schema",
                     file.string() + " is not a supple.favourites/1.0 document", "/schema");
    const json::Value* list = doc->find("favourites");
    if (!list) return out;
    if (!list->isArray())
        return Error("favourites.list", "favourites must be an array of plugin ids", "/favourites");
    for (const json::Value& id : list->asArray()) {
        if (!id.isString())
            return Error("favourites.id", "every favourite is a plugin id", "/favourites");
        out.ids.push_back(id.asString());
    }
    return out;
}

Status Favourites::save(const std::filesystem::path& file) const {
    json::Value doc = json::Value::object();
    doc["schema"] = "supple.favourites/1.0";
    json::Value list = json::Value::array();
    for (const std::string& id : ids) list.push_back(id);
    doc["favourites"] = std::move(list);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    return writeFileAtomic(file, json::writeCanonical(doc) + "\n");
}

bool Favourites::has(std::string_view id) const {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

bool Favourites::toggle(std::string_view id) {
    auto it = std::find(ids.begin(), ids.end(), id);
    if (it != ids.end()) {
        ids.erase(it);
        return false;
    }
    ids.emplace_back(id);
    return true;
}

// --- PluginBrowser --------------------------------------------------------------

PluginBrowser::PluginBrowser(engine::EngineApp&, cmd::Dispatcher& dispatcher, ui::Root& root,
                             std::filesystem::path favouritesFile)
    : dispatcher_(dispatcher), root_(root), favouritesFile_(std::move(favouritesFile)) {
    if (auto loaded = Favourites::load(favouritesFile_))
        favourites_ = std::move(*loaded);
    else
        std::fprintf(stderr, "favourites: %s\n", loaded.error().toString().c_str());
}

std::unique_ptr<ui::Node> PluginBrowser::buildPage() {
    auto id = [this](const char* name) { return prefix_ + "." + name; };
    auto page = std::make_unique<ui::Node>(prefix_);
    page_ = page.get();
    page->layout.dir = ui::Layout::Dir::Column;
    page->layout.padding = ui::Insets::all(12);
    page->layout.gap = 8;
    if (!embedded_) {  // a popup has its own size, title and Close
        page->layout.width = ui::SizeRule::fixed(560);
        page->layout.height = ui::SizeRule::fixed(440);
        auto& header = page->emplace<ui::Node>(id("header"));
        header.layout.dir = ui::Layout::Dir::Row;
        header.layout.height = ui::SizeRule::fixed(28);
        header.layout.align = ui::Layout::Align::Center;
        title_ = &header.emplace<ui::Label>(id("title"), titleText_, ui::Font::UiBold, "body");
        title_->layout.width = ui::SizeRule::fraction();
        header.emplace<ui::Button>(id("close"), "Close", [this] { close(); });
    }
    search_ = &page->emplace<ui::TextField>(id("search"), "", "Search plugins…");
    search_->layout.height = ui::SizeRule::fixed(26);
    list_ = &page->emplace<ui::List>(id("results"));
    list_->rowHeight = embedded_ ? 24 : 30;
    list_->paintRow = [this](ui::Canvas& c, std::size_t i, const ui::Rect& r, bool) {
        if (i >= rows_.size()) return;
        const ui::Theme& t = c.theme();
        const RowText text = rowText(i);
        const bool shadowed = rows_[i].entry->shadowed;
        if (embedded_) {
            // A narrow column: the name, and whose it is at the right. The
            // id is in the line under the list.
            const std::string whose =
                text.format.empty() ? text.vendor : text.vendor + " · " + text.format;
            const float vendor = c.textWidth(whose, ui::Font::Ui, t.type("caption"));
            c.text({r.x + 8, r.y, std::max(0.0f, r.w - 28 - vendor), r.h}, text.name, ui::Font::Ui,
                   t.type("small"), shadowed ? t.color("ink-quiet") : t.color("ink"));
            c.text({r.x, r.y, r.w - 10, r.h}, whose, ui::Font::Ui, t.type("caption"),
                   shadowed ? t.color("clip") : t.color("ink-faint"), ui::kRight | ui::kMiddle);
            return;
        }
        const float w = c.textWidth(text.name, ui::Font::Ui, t.type("small"));
        c.text({r.x + 10, r.y, r.w - 20, r.h}, text.name, ui::Font::Ui, t.type("small"),
               shadowed ? t.color("ink-quiet") : t.color("ink"));
        c.text({r.x + 20 + w, r.y, std::max(0.0f, r.w - 32 - w), r.h}, text.vendor, ui::Font::Mono,
               t.type("caption"), shadowed ? t.color("clip") : t.color("ink-faint"));
        c.text({r.x, r.y, r.w - 12, r.h}, text.id, ui::Font::Mono, t.type("caption"),
               t.color("ink-quiet"), ui::kRight | ui::kMiddle);
    };
    search_->onChange = [this](const std::string&) { sortAndFilter(); };
    search_->onSubmit = [this](const std::string&) { activateChosen(); };
    search_->onOtherKey = [this](const ui::KeyEvent& e) { return list_->onKey(e); };
    list_->onActivate = [this](std::size_t row) { activate(row); };
    list_->onSelect = [this](std::size_t) { updateDetail(); };
    if (onDrag)
        list_->onDrag = [this](std::size_t row, ui::DragPhase phase, ui::Point at) {
            // The plugin is the one the drag began on, whatever the rows
            // have become since (a search, a rescan).
            if (phase == ui::DragPhase::Begin) {
                if (row >= rows_.size()) return;
                dragged_ = {rows_[row].entry->id, rows_[row].entry->name};
            }
            if (dragged_.first.empty()) return;
            const auto [plugin, name] = dragged_;
            if (phase == ui::DragPhase::End || phase == ui::DragPhase::Cancel) dragged_ = {};
            if (auto drag = onDrag) drag(plugin, name, phase, at);
        };
    auto& footer = page->emplace<ui::Node>(id("footer"));
    footer.layout.dir = ui::Layout::Dir::Row;
    footer.layout.height = ui::SizeRule::fixed(28);
    footer.layout.gap = 6;
    footer.layout.align = ui::Layout::Align::Center;
    auto& rescan = footer.emplace<ui::Button>(id("rescan"), "Rescan", [this] {
        runScan();
        root_.damageAll();
    });
    auto& favourite = footer.emplace<ui::Button>(id("fav"), "Favourite", [this] {
        const auto row = list_->selected();
        if (!row || *row >= rows_.size()) return;
        const std::string plugin = rows_[*row].entry->id;
        // Another open browser may have saved since this one was opened.
        auto latest = Favourites::load(favouritesFile_);
        if (!latest) {
            setStatus("favourites: " + latest.error().message);
            return;
        }
        favourites_ = std::move(*latest);
        const bool now = favourites_.toggle(plugin);
        if (auto saved = favourites_.save(favouritesFile_); !saved)
            setStatus("favourites: " + saved.error().message);
        else
            setStatus(now ? "Added to favourites" : "Removed from favourites");
        sortAndFilter();
        root_.damageAll();
    });
    // In a panel the secondary buttons are quiet (§7) and Add is at the right.
    rescan.quiet = favourite.quiet = embedded_;
    if (embedded_)
        footer.emplace<ui::Node>(id("footer.rest")).layout.width = ui::SizeRule::fraction();
    add_ = &footer.emplace<ui::Button>(id("add"), "Add", [this] { activateChosen(); });
    if (embedded_) {
        // The status has a line of its own: the footer is three buttons wide.
        status_ = &page->emplace<ui::Label>(id("status"), "", ui::Font::Ui, "caption", "ink-quiet");
        status_->layout.height = ui::SizeRule::fixed(16);
    } else {
        status_ =
            &footer.emplace<ui::Label>(id("status"), "", ui::Font::Ui, "caption", "ink-quiet");
        status_->layout.width = ui::SizeRule::fixed(120);
    }
    detailLabel_ =
        &page->emplace<ui::Label>(id("detail"), "", ui::Font::Mono, "caption", "ink-quiet");
    detailLabel_->layout.width = ui::SizeRule::fraction();
    if (embedded_) detailLabel_->layout.height = ui::SizeRule::fixed(16);
    refresh();
    return page;
}

std::unique_ptr<ui::Node> PluginBrowser::buildEmbedded(
    std::string prefix, std::function<void(const std::string&)> onAdd) {
    prefix_ = std::move(prefix);
    embedded_ = true;
    onAdd_ = std::move(onAdd);
    return buildPage();
}

void PluginBrowser::reload() {
    if (auto loaded = Favourites::load(favouritesFile_)) favourites_ = std::move(*loaded);
    refresh();
}

void PluginBrowser::focusSearch() {
    if (search_) root_.focus(search_);
}

PluginBrowser::RowText PluginBrowser::rowText(std::size_t row) const {
    RowText text;
    if (row >= rows_.size()) return text;
    const Entry& entry = *rows_[row].entry;
    text.name = (rows_[row].favourite ? std::string(kFavMark) + " " : "") + entry.name;
    if (!entry.kind.empty()) text.name += " (" + entry.kind + ")";
    text.vendor = entry.shadowed                                   ? "shadowed"
                  : lower(entry.vendor) == lower(product::kVendor) ? "stock"
                                                                   : entry.vendor;
    text.id = entry.id;
    if (entry.format == "vst3") text.format = "VST3";
    return text;
}

// The selected plugin's own file: a shadowed copy is the one a `fx.add` would
// not use, which is why it is called out.
void PluginBrowser::activateChosen() {
    if (const auto row = list_->selected())
        activate(*row);
    else if (!embedded_)
        activate(0);
}

void PluginBrowser::updateDetail() {
    // In the panel Add waits for a chosen row.
    if (add_) add_->enabled = !embedded_ || (list_ && list_->selected().has_value());
    std::string text;
    if (const auto row = list_ ? list_->selected() : std::nullopt; row && *row < rows_.size()) {
        const Entry& entry = *rows_[*row].entry;
        text = entry.path;
        if (entry.shadowed) text += "  — shadowed: the first copy on the plugin path wins";
    }
    detail_ = text;
    if (!detailLabel_) return;
    // A panel's line is too short for a path: it names the plugin as a
    // command would (`fx.add {plugin}`).
    const auto row = list_ ? list_->selected() : std::nullopt;
    detailLabel_->setText(
        embedded_ ? (row && *row < rows_.size() ? rows_[*row].entry->id : std::string()) : detail_);
}

void PluginBrowser::open(std::function<void(const std::string&)> onAdd, std::string title) {
    if (auto loaded = Favourites::load(favouritesFile_)) favourites_ = std::move(*loaded);
    onAdd_ = std::move(onAdd);
    titleText_ = title.empty() ? std::string("Add plugin") : std::move(title);
    if (page_) {  // already open: raise it
        if (title_) title_->setText(titleText_);
        refresh();
        return;
    }
    auto page = buildPage();
    root_.openPopup(std::move(page), {}, ui::Placement::Center, [this] {
        onAdd_ = {};
        page_ = nullptr;
        search_ = nullptr;
        list_ = nullptr;
        add_ = nullptr;
        status_ = nullptr;
        detailLabel_ = nullptr;
        title_ = nullptr;
    });
    if (search_) root_.focus(search_);  // typing searches (not the header's Close)
}

void PluginBrowser::close() {
    if (page_ && !embedded_) root_.closePopup();
}

void PluginBrowser::setStatus(std::string message) {
    if (status_) status_->setText(std::move(message));
}

// The scan runs in the background (every module in the supervised scanner);
// tick() follows its job and shows the new catalogue when it is done.
void PluginBrowser::tick() {
    if (scanJob_.empty() || !page_) return;
    json::Value args = json::Value::object();
    args["job"] = scanJob_;
    auto state = dispatcher_.execute({"job.status", std::move(args), kUiOrigin});
    const json::Value* status = state && state->isObject() ? state->find("status") : nullptr;
    if (status && status->isString() && status->asString() == "running") return;
    scanJob_.clear();
    if (status && status->isString() && status->asString() == "done") {
        refresh();
        setStatus("Rescan complete");
        return;
    }
    const json::Value* error = state && state->isObject() ? state->find("error") : nullptr;
    const json::Value* message = error && error->isObject() ? error->find("message") : nullptr;
    setStatus(message && message->isString() ? "Rescan failed: " + message->asString()
              : state                        ? std::string("Rescan stopped")
                                             : state.error().message);
}

void PluginBrowser::runScan() {
    json::Value args = json::Value::object();
    args["wait"] = false;
    auto result = dispatcher_.execute({"plugin.scan", std::move(args), kUiOrigin});
    const json::Value* job = result && result->isObject() ? result->find("job") : nullptr;
    if (!job || !job->isString()) {
        setStatus(result ? std::string("Rescan did not start") : result.error().message);
        return;
    }
    scanJob_ = job->asString();
    setStatus("Scanning…");
}

void PluginBrowser::refresh() {
    auto result = dispatcher_.execute({"plugin.list", json::Value::object(), kUiOrigin});
    if (!result) {
        setStatus(result.error().message);
        return;
    }
    all_.clear();
    const json::Value* plugins = result->isObject() ? result->find("plugins") : nullptr;
    if (plugins && plugins->isArray())
        for (const json::Value& p : plugins->asArray()) {
            Entry e;
            const auto str = [&](const char* key) {
                const json::Value* v = p.find(key);
                return v && v->isString() ? v->asString() : std::string();
            };
            e.id = str("id");
            e.name = str("name");
            e.vendor = str("vendor");
            e.format = str("format");
            e.instrument = p.find("instrument") && p.find("instrument")->asBool();
            e.semantic = p.find("semantic") && p.find("semantic")->asBool();
            e.path = str("path");
            e.shadowed = p.find("shadowed") && p.find("shadowed")->asBool();
            std::vector<std::string> features;
            if (const json::Value* f = p.find("features"); f && f->isArray())
                for (const json::Value& x : f->asArray())
                    if (x.isString()) features.push_back(x.asString());
            e.kind = stockKind(e.vendor, features);
            all_.push_back(std::move(e));
        }
    sortAndFilter();
    root_.damageAll();
}

void PluginBrowser::sortAndFilter() {
    const std::string query = search_ ? search_->text() : std::string();
    rows_.clear();
    for (const Entry& e : all_) {
        if (!query.empty() && !contains(e.id, query) && !contains(e.name, query) &&
            !contains(e.vendor, query) && !contains(e.kind, query))
            continue;
        rows_.push_back(Row{&e, favourites_.has(e.id)});
    }
    auto rank = [&](const Row& r) {
        const bool stock = lower(r.entry->vendor) == lower(product::kVendor);
        return std::pair{r.favourite ? 0 : 1, stock ? 0 : 1};
    };
    std::stable_sort(rows_.begin(), rows_.end(), [&](const Row& a, const Row& b) {
        if (rank(a) != rank(b)) return rank(a) < rank(b);
        const int byName = lower(a.entry->name).compare(lower(b.entry->name));
        return byName != 0 ? byName < 0 : a.entry->id < b.entry->id;
    });
    shown_.clear();
    for (const Row& r : rows_) shown_.push_back(r.entry->id);
    if (list_) {
        list_->setCount(rows_.size());
        // The popup opens for one track and starts on its first row. In the
        // panel Add goes to every selected track, so a row is chosen first:
        // by a click, the arrow keys, or a search (its first hit).
        const bool first = !rows_.empty() && (!embedded_ || (search_ && !search_->text().empty()));
        list_->select(first ? std::optional<std::size_t>(0) : std::nullopt);
        list_->invalidate();
    }
    updateDetail();
}

void PluginBrowser::activate(std::size_t row) {
    if (row >= rows_.size()) return;
    const std::string id = rows_[row].entry->id;
    auto add = onAdd_;
    if (!embedded_) close();  // the popup is destroyed here; a panel's browser stays
    if (add) add(id);
}

}  // namespace supple::gui
