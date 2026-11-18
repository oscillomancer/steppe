// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "side_browser.h"

#include <SDL3/SDL_keycode.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

#include "fx_window.h"
#include "plugin_browser.h"
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
constexpr std::array<Kind, 2> kKinds{{{"plugins", "Plug-ins"}, {"chains", "Chains"}}};

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
    return page;
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
        addChain(chainList_->selected().value_or(0));
    };
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
    footer.emplace<ui::Button>("side.browser.chains.add", "Add",
                               [this] { addChain(chainList_->selected().value_or(0)); });
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

void SideBrowser::setVisible(bool visible) {
    visible_ = visible;
    if (visible) reload();
}

void SideBrowser::show(const std::string& kind) {
    const bool known =
        std::any_of(kKinds.begin(), kKinds.end(), [&](const Kind& k) { return kind == k.id; });
    if (!known || kind == kind_ || !pluginsPage_) return;
    // The page leaving view gives up the keyboard.
    root_.releaseWithin(kind_ == "plugins" ? pluginsPage_ : chainsPage_);
    kind_ = kind;
    pluginsPage_->visible = kind_ == "plugins";
    chainsPage_->visible = kind_ == "chains";
    if (visible_) reload();
    root_.relayout();
    root_.damageAll();
}

void SideBrowser::focusSearch() {
    if (kind_ == "plugins")
        fx_.sideBrowser().focusSearch();
    else if (chainSearch_)
        root_.focus(chainSearch_);
}

void SideBrowser::reload() {
    if (kind_ == "plugins") {
        fx_.sideBrowser().reload();
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
    chainList_->select(shown_.empty() ? std::nullopt : std::optional<std::size_t>(0));
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
