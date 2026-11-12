// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "rack_window.h"

#include <SDL3/SDL_keycode.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

#include "learn_dialog.h"
#include "plugin_browser.h"
#include "supple/engine/midi_control.h"
#include "supple/rack/host_modules.h"
#include "supple/ui/list.h"
#include "supple/ui/popup.h"
#include "supple/ui/text_area.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

const cmd::Origin kOrigin{cmd::Origin::Type::Ui, "gui", {}};
void openNameField(ui::Root& root, std::string id, std::string text, ui::Rect where,
                   std::function<void(const std::string&)> submit);
// A browser entry for a rack patch (a saved preset or a factory patch):
// "preset:<name>".
constexpr std::string_view kPresetPrefix = "preset:";
constexpr float kBrowserW = 220, kHeaderH = 34, kStatusH = 22;
constexpr auto kTelemetryInterval = std::chrono::milliseconds(33);

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string str(const json::Value& v, const char* key) {
    const json::Value* x = v.find(key);
    return x && x->isString() ? x->asString() : std::string();
}

}  // namespace

// --- the tabs ------------------------------------------------------------------------

class RackTabs : public ui::Node {
  public:
    struct Tab {
        model::Id rack;
        std::string label;
        std::optional<ui::Color> color;  // the home channel's
    };
    struct Hooks {
        std::function<void(model::Id)> select;
        std::function<void(model::Id)> rename;
        std::function<void(model::Id, ui::Point)> menu;
        std::function<void()> add;
    };
    explicit RackTabs(Hooks hooks) : Node("rack.tabs", ui::Role::Tab), hooks_(std::move(hooks)) {
        interactive = true;
        layout.width = ui::SizeRule::fraction();
        layout.height = ui::SizeRule::fixed(kHeaderH);
    }
    void set(std::vector<Tab> tabs, std::optional<model::Id> current) {
        tabs_ = std::move(tabs);
        current_ = current;
        invalidate();
    }
    std::string valueText() const override {
        std::string out;
        for (const auto& t : tabs_)
            out +=
                (out.empty() ? "" : " | ") + std::string(current_ == t.rack ? "*" : "") + t.label;
        return out;
    }
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        const ui::Rect b = bounds();
        boxes_.clear();
        float x = b.x + 6;
        for (const auto& tab : tabs_) {
            const float w =
                std::clamp(c.textWidth(tab.label, ui::Font::UiBold, 12) + 34, 76.0f, 220.0f);
            const ui::Rect r{x, b.y + 6, w, b.h - 6};
            boxes_.push_back(r);
            const bool on = current_ == tab.rack, over = hoverAt_ && r.contains(*hoverAt_);
            if (on) {
                c.fillRound({r.x, r.y, r.w, r.h + 6}, 5, t.color("ground"));
            } else if (over) {
                c.fillRound({r.x, r.y + 2, r.w, r.h - 4}, 5, t.color("raised"));
            }
            const ui::Color dot = tab.color.value_or(t.color("channel"));
            c.fillCircle({r.x + 13, r.y + r.h / 2}, 3.5f, on ? dot : dot.withAlpha(0.6f));
            c.text({r.x + 22, r.y, r.w - 28, r.h}, tab.label, on ? ui::Font::UiBold : ui::Font::Ui,
                   12, on ? t.color("ink") : t.color("ink-quiet"));
            x += w + 2;
        }
        plus_ = {x + 2, b.y + 8, 26, b.h - 12};
        const bool over = hoverAt_ && plus_.contains(*hoverAt_);
        if (over) c.fillRound(plus_, 4, t.color("raised"));
        const ui::Point m{plus_.x + plus_.w / 2, plus_.y + plus_.h / 2};
        const ui::Color ink = over ? t.color("ink") : t.color("ink-quiet");
        c.line({m.x - 5, m.y}, {m.x + 5, m.y}, ink, 1.5f);
        c.line({m.x, m.y - 5}, {m.x, m.y + 5}, ink, 1.5f);
    }
    bool onPointer(const ui::PointerEvent& e) override {
        using T = ui::PointerEvent::Type;
        if (e.type == T::Move || e.type == T::Enter) {
            hoverAt_ = e.pos;
            invalidate();
            return true;
        }
        if (e.type == T::Leave) {
            hoverAt_.reset();
            invalidate();
            return true;
        }
        if (e.type != T::Down) return false;
        if (plus_.contains(e.pos) && e.button == 1) {
            if (hooks_.add) hooks_.add();
            return true;
        }
        for (std::size_t i = 0; i < boxes_.size() && i < tabs_.size(); ++i) {
            if (!boxes_[i].contains(e.pos)) continue;
            const model::Id id = tabs_[i].rack;
            if (e.button == 3) {
                if (hooks_.select) hooks_.select(id);
                if (hooks_.menu) hooks_.menu(id, e.pos);
            } else if (e.button == 1) {
                if (hooks_.select) hooks_.select(id);
                if (e.clicks >= 2 && hooks_.rename) hooks_.rename(id);
            }
            return true;
        }
        return true;
    }
    std::optional<ui::Rect> tabRect(model::Id id) const {
        for (std::size_t i = 0; i < boxes_.size() && i < tabs_.size(); ++i)
            if (tabs_[i].rack == id) return boxes_[i];
        return std::nullopt;
    }
    ui::Rect plusRect() const { return plus_; }

  private:
    Hooks hooks_;
    std::vector<Tab> tabs_;
    std::optional<model::Id> current_;
    std::vector<ui::Rect> boxes_;
    ui::Rect plus_;
    std::optional<ui::Point> hoverAt_;
};

// --- the browser ---------------------------------------------------------------------

class RackBrowser : public ui::Node {
  public:
    struct Entry {
        std::string id, name, description;
        std::vector<std::string> tags;
        rack::Origin origin = rack::Origin::Stock;
        // A rack patch ("preset:<name>"), listed in the Patches view: the
        // user's saved preset, or a factory patch that ships with the app.
        bool preset = false, factory = false;
        std::string detail;  // the hover card's last line (what it holds)
    };
    struct Hooks {
        std::function<void(const std::string& type)> add;
        // A right-click on a row.
        std::function<void(const std::string& type, ui::Point at)> menu;
        // A row dragged out over the window (none: back over the list or done).
        std::function<void(const std::string& name, std::optional<ui::Point> at)> dragOver;
        std::function<void(const std::string& type, ui::Point at)> drop;
    };
    explicit RackBrowser(Hooks hooks) : Node("rack.browser"), hooks_(std::move(hooks)) {
        layout.dir = ui::Layout::Dir::Column;
        layout.width = ui::SizeRule::fixed(kBrowserW);
        layout.padding = {10, 10, 10, 6};
        layout.gap = 8;
        views_ = &emplace<Views>(*this);
        search_ = &emplace<ui::TextField>("rack.browser.search", "", "Search modules");
        search_->layout.height = ui::SizeRule::fixed(26);
        search_->onChange = [this](const std::string&) { filter(); };
        search_->onOtherKey = [this](const ui::KeyEvent& e) { return list_->key(e); };
        search_->onSubmit = [this](const std::string&) { list_->activate(); };
        tags_ = &emplace<Tags>(*this);
        list_ = &emplace<List>(*this);
    }
    void setEntries(std::vector<Entry> entries) {
        entries_ = std::move(entries);
        std::map<std::string, int> tags;
        for (const auto& e : entries_)
            for (const auto& t : e.tags) ++tags[t];
        allTags_.clear();
        for (const auto& [t, n] : tags) allTags_.push_back(t);
        // The folded row's order: the most used first.
        commonTags_ = allTags_;
        std::stable_sort(
            commonTags_.begin(), commonTags_.end(),
            [&](const std::string& a, const std::string& b) { return tags[a] > tags[b]; });
        if (tag_ && !tags.contains(*tag_)) tag_.reset();
        filter();
        tags_->relayout();
    }
    std::vector<std::string> shown() const {
        std::vector<std::string> out;
        for (const auto& r : rows_)
            if (r.entry) out.push_back(entries_[*r.entry].id);
        return out;
    }
    // The Patches view (the user's and the factory's rack patches) or the
    // Modules view (the module types: the default, so what it shows does
    // not move as patches come and go).
    void setPatches(bool on) {
        hideCard();
        if (on == patches_) return;
        patches_ = on;
        search_->setText("");
        search_->setPlaceholder(on ? "Search patches" : "Search modules");
        tags_->visible = !on;
        filter();
        tags_->relayout();
        views_->invalidate();
    }
    bool patches() const { return patches_; }
    // The hover card away (the browser hidden).
    void dismissCard() { hideCard(); }
    void paint(ui::Canvas& c) override {
        c.fillRect(bounds(), c.theme().color("panel"));
        c.fillRect({bounds().right() - 1, bounds().y, 1, bounds().h}, c.theme().color("rule"));
    }
    void setTag(std::optional<std::string> tag) {
        tag_ = std::move(tag);
        filter();
        tags_->relayout();
    }
    std::optional<std::string> tag() const { return tag_; }
    void focusSearch() {
        if (root()) root()->focus(search_);
        search_->selectAll();
    }
    // Tests: the switch's halves, and a shown row (by id), as window points.
    ui::Point viewAt(bool patches) const { return views_->at(patches); }
    std::optional<ui::Point> rowAt(const std::string& id) const {
        for (std::size_t i = 0; i < rows_.size(); ++i)
            if (rows_[i].entry && entries_[*rows_[i].entry].id == id) {
                const float y = list_->rowTop(i);
                if (y < list_->bounds().y || y + 18 > list_->bounds().bottom()) return std::nullopt;
                return ui::Point{list_->bounds().x + 40, y + 18};
            }
        return std::nullopt;
    }
    // The tags: one row (the chosen tag, then the most used) and "+N" for
    // the rest, or all of them, alphabetically, with "Less".
    void setTagsOpen(bool open) {
        tagsOpen_ = open;
        tags_->relayout();
    }
    bool tagsOpen() const { return tagsOpen_; }
    // Tests: the chips shown, in order ("+N" and "Less" included), and where.
    std::vector<std::string> tagChips() const { return tags_->chips(); }
    std::optional<ui::Point> tagChipAt(const std::string& label) const { return tags_->at(label); }

  private:
    struct Row {
        std::optional<std::size_t> entry;  // none: an origin's heading
        std::string heading;
    };
    void filter() {
        rows_.clear();
        std::vector<std::string> words;
        {
            std::string q = lower(search_->text()), w;
            for (char ch : q + " ")
                if (ch == ' ') {
                    if (!w.empty()) words.push_back(w);
                    w.clear();
                } else {
                    w += ch;
                }
        }
        std::optional<std::string> group;
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            const Entry& e = entries_[i];
            if (e.preset != patches_) continue;
            if (!patches_ && tag_ && std::find(e.tags.begin(), e.tags.end(), *tag_) == e.tags.end())
                continue;
            std::string hay = lower(e.name + " " + e.id + " " + e.description);
            for (const auto& t : e.tags) hay += " " + t;
            if (!std::all_of(words.begin(), words.end(), [&](const std::string& w) {
                    return hay.find(w) != std::string::npos;
                }))
                continue;
            const std::string heading = !e.preset   ? std::string(rack::originName(e.origin))
                                        : e.factory ? std::string("factory")
                                                    : std::string("yours");
            if (group != heading) {
                group = heading;
                rows_.push_back({std::nullopt, heading});
            }
            rows_.push_back({i, {}});
        }
        hideCard();
        list_->reset();
    }

    // The two views, side by side above the search.
    class Views : public ui::Node {
      public:
        explicit Views(RackBrowser& b) : Node("rack.browser.views", ui::Role::Tab), b_(b) {
            interactive = true;
            layout.height = ui::SizeRule::fixed(24);
        }
        std::string valueText() const override { return b_.patches_ ? "Patches" : "Modules"; }
        void paint(ui::Canvas& c) override {
            const ui::Theme& t = c.theme();
            const ui::Rect b = bounds();
            c.fillRound(b, 5, t.color("ground"));
            for (bool patches : {false, true}) {
                const ui::Rect r = half(patches);
                const bool on = b_.patches_ == patches;
                if (on) c.fillRound(r.inset(2), 4, t.color("raised"));
                c.text(r, patches ? "Patches" : "Modules", on ? ui::Font::UiBold : ui::Font::Ui,
                       11.5f, on ? t.color("ink") : t.color("ink-quiet"),
                       ui::kCenter | ui::kMiddle);
            }
        }
        bool onPointer(const ui::PointerEvent& e) override {
            if (e.type != ui::PointerEvent::Type::Down || e.button != 1)
                return e.type != ui::PointerEvent::Type::Move;
            b_.setPatches(half(true).contains(e.pos));
            return true;
        }
        ui::Point at(bool patches) const {
            const ui::Rect r = half(patches);
            return {r.x + r.w / 2, r.y + r.h / 2};
        }

      private:
        ui::Rect half(bool patches) const {
            const ui::Rect b = bounds();
            return {b.x + (patches ? b.w / 2 : 0), b.y, b.w / 2, b.h};
        }
        RackBrowser& b_;
    };

    // A row's whole description, beside the browser while the row is hovered.
    class Card : public ui::Node {
      public:
        static constexpr float kW = 260, kPad = 10;
        Card() : Node("rack.browser.card") {}
        std::string name, description, detail;
        ui::Point anchor;  // its top-left, before it is kept in the window
        std::string valueText() const override {
            return name + ": " + description + (detail.empty() ? "" : " (" + detail + ")");
        }
        void arrange(const ui::Canvas& c, const ui::Rect&) override {
            const ui::Rect bounds = parent() ? parent()->bounds() : ui::Rect{0, 0, 1e6f, 1e6f};
            const float w = kW - 2 * kPad;
            float h = kPad + 16;
            h += static_cast<float>(c.wrap(description, ui::Font::Ui, 11, w).size()) * 15;
            if (!detail.empty())
                h += 4 + static_cast<float>(c.wrap(detail, ui::Font::Ui, 10, w).size()) * 13;
            h += kPad;
            const float y =
                std::clamp(anchor.y, bounds.y + 4, std::max(bounds.y + 4, bounds.bottom() - h - 4));
            setBounds({anchor.x, y, kW, h});
        }
        void paint(ui::Canvas& c) override {
            const ui::Theme& t = c.theme();
            const ui::Rect b = bounds();
            c.fillRound({b.x + 1, b.y + 3, b.w, b.h}, 6, ui::Color{0, 0, 0, 0.3f});
            c.fillRound(b, 6, t.color("raised"));
            c.strokeRound(b.inset(0.5f), 6, t.color("rule"), 1);
            const float w = b.w - 2 * kPad;
            float y = b.y + kPad;
            c.text({b.x + kPad, y, w, 16}, name, ui::Font::UiBold, 12, t.color("ink"),
                   ui::kLeft | ui::kMiddle);
            y += 16;
            for (const auto& line : c.wrap(description, ui::Font::Ui, 11, w)) {
                c.text({b.x + kPad, y, w, 15}, line, ui::Font::Ui, 11, t.color("ink-quiet"),
                       ui::kLeft | ui::kMiddle);
                y += 15;
            }
            if (detail.empty()) return;
            y += 4;
            for (const auto& line : c.wrap(detail, ui::Font::Ui, 10, w)) {
                c.text({b.x + kPad, y, w, 13}, line, ui::Font::Ui, 10, t.color("ink-faint"),
                       ui::kLeft | ui::kMiddle);
                y += 13;
            }
        }
    };
    void showCard(std::size_t row, float y) {
        if (row >= rows_.size() || !rows_[row].entry || !root()) return hideCard();
        const Entry& e = entries_[*rows_[row].entry];
        if (e.description.empty() && e.detail.empty()) return hideCard();
        if (!card_) {
            auto made = std::make_unique<Card>();
            cardNode_ = made.get();
            card_ = root()->addOverlay(std::move(made));
        }
        cardNode_->name = e.name;
        cardNode_->description = e.description;
        cardNode_->detail = e.detail;
        cardNode_->anchor = {bounds().right() + 6, y};
        cardNode_->relayout();
        cardNode_->invalidate();
    }
    void hideCard() {
        card_.reset();
        cardNode_ = nullptr;
    }

    class Tags : public ui::Node {
      public:
        explicit Tags(RackBrowser& b) : Node("rack.browser.tags"), b_(b) {
            interactive = true;
            layout.height = ui::SizeRule::content();
        }
        ui::Size measure(const ui::Canvas& c, ui::Size available) override {
            return {available.w, flow(c, available.w, nullptr)};
        }
        void paint(ui::Canvas& c) override {
            boxes_.clear();
            flow(c, bounds().w, &c);
        }
        bool onPointer(const ui::PointerEvent& e) override {
            if (e.type != ui::PointerEvent::Type::Down || e.button != 1)
                return e.type != ui::PointerEvent::Type::Move;
            for (const auto& [r, chip] : boxes_)
                if (r.contains(e.pos)) {
                    if (chip.kind == Chip::Kind::More)
                        b_.setTagsOpen(true);
                    else if (chip.kind == Chip::Kind::Less)
                        b_.setTagsOpen(false);
                    else {  // a tag picked from the whole set folds it again
                        b_.tagsOpen_ = false;
                        b_.setTag(b_.tag_ == chip.label ? std::nullopt : std::optional(chip.label));
                    }
                    return true;
                }
            return true;
        }
        std::vector<std::string> chips() const {
            std::vector<std::string> out;
            if (!visible) return out;
            for (const auto& [r, chip] : boxes_) out.push_back(chip.label);
            return out;
        }
        std::optional<ui::Point> at(const std::string& label) const {
            for (const auto& [r, chip] : boxes_)
                if (chip.label == label) return ui::Point{r.x + r.w / 2, r.y + r.h / 2};
            return std::nullopt;
        }
        std::string valueText() const override {
            std::string out;
            for (const auto& c : chips()) out += (out.empty() ? "" : ", ") + c;
            return out;
        }

      private:
        struct Chip {
            enum class Kind { Tag, More, Less } kind = Kind::Tag;
            std::string label;
        };
        static constexpr float kH = 18, kGap = 4, kFont = 10.5f;
        static float widthOf(const ui::Canvas& c, const std::string& label) {
            return c.textWidth(label, ui::Font::Ui, kFont) + 14;
        }
        // The chips shown: folded, one row's worth; open, every tag.
        std::vector<Chip> chipsFor(const ui::Canvas& c, float width) const {
            std::vector<Chip> out;
            const auto& all = b_.allTags_;
            if (all.empty()) return out;
            if (b_.tagsOpen_) {
                for (const auto& t : all) out.push_back({Chip::Kind::Tag, t});
                out.push_back({Chip::Kind::Less, "Less"});
                return out;
            }
            std::vector<std::string> order;
            if (b_.tag_) order.push_back(*b_.tag_);
            for (const auto& t : b_.commonTags_)
                if (t != b_.tag_) order.push_back(t);
            const float more = widthOf(c, "+" + std::to_string(all.size())) + kGap;
            float x = 0;
            std::size_t shown = 0;
            for (const auto& t : order) {
                const float w = widthOf(c, t);
                const bool last = shown + 1 == order.size();
                if (x + w > width - (last ? 0 : more)) break;
                out.push_back({Chip::Kind::Tag, t});
                x += w + kGap;
                ++shown;
            }
            if (shown < order.size())
                out.push_back({Chip::Kind::More, "+" + std::to_string(order.size() - shown)});
            return out;
        }
        // Lays out (and with `paint` paints) the chips; returns their height.
        float flow(const ui::Canvas& c, float width, ui::Canvas* paint) {
            const std::vector<Chip> chips = chipsFor(c, width);
            float x = 0, y = 0;
            for (const Chip& chip : chips) {
                const float w = widthOf(c, chip.label);
                if (x > 0 && x + w > width) {
                    x = 0;
                    y += kH + kGap;
                }
                if (paint) {
                    const ui::Theme& t = paint->theme();
                    const ui::Rect r{bounds().x + x, bounds().y + y, w, kH};
                    const bool on = chip.kind == Chip::Kind::Tag && b_.tag_ == chip.label;
                    if (chip.kind == Chip::Kind::Tag)
                        paint->fillRound(r, kH / 2, on ? t.color("accent") : t.color("raised"));
                    else  // "+N" and "Less": outlined, not filled
                        paint->strokeRound(r, kH / 2, t.color("rule"), 1);
                    paint->text(r, chip.label, ui::Font::Ui, kFont,
                                on                             ? t.color("ground")
                                : chip.kind == Chip::Kind::Tag ? t.color("ink-quiet")
                                                               : t.color("ink"),
                                ui::kCenter | ui::kMiddle);
                    boxes_.emplace_back(r, chip);
                }
                x += w + kGap;
            }
            return chips.empty() ? 0 : y + kH;
        }
        RackBrowser& b_;
        std::vector<std::pair<ui::Rect, Chip>> boxes_;
    };

    class List : public ui::Node {
      public:
        explicit List(RackBrowser& b) : Node("rack.browser.list", ui::Role::List), b_(b) {
            interactive = true;
            clipsChildren = true;
            layout.height = ui::SizeRule::fraction();
        }
        void reset() {
            scroll_ = 0;
            selected_.reset();
            for (std::size_t i = 0; i < b_.rows_.size(); ++i)
                if (b_.rows_[i].entry) {
                    if (!b_.search_->text().empty() || b_.tag_) selected_ = i;
                    break;
                }
            invalidate();
        }
        std::string valueText() const override {
            return std::to_string(b_.shown().size()) + (b_.patches_ ? " patches" : " modules");
        }
        void onHover(bool on) override {
            if (on) return;
            hover_.reset();
            b_.hideCard();
            invalidate();
        }
        bool key(const ui::KeyEvent& e) {
            if (e.key != SDLK_DOWN && e.key != SDLK_UP) return false;
            const int dir = e.key == SDLK_DOWN ? 1 : -1;
            long i = selected_ ? static_cast<long>(*selected_) : -1;
            for (i += dir; i >= 0 && i < static_cast<long>(b_.rows_.size()); i += dir)
                if (b_.rows_[static_cast<std::size_t>(i)].entry) {
                    selected_ = static_cast<std::size_t>(i);
                    break;
                }
            invalidate();
            return true;
        }
        void activate() {
            if (selected_ && *selected_ < b_.rows_.size() && b_.rows_[*selected_].entry &&
                b_.hooks_.add)
                b_.hooks_.add(b_.entries_[*b_.rows_[*selected_].entry].id);
        }
        void paint(ui::Canvas& c) override {
            const ui::Theme& t = c.theme();
            const ui::Rect b = bounds();
            c.save();
            c.clip(b);
            float y = b.y - scroll_;
            for (std::size_t i = 0; i < b_.rows_.size(); ++i) {
                const Row& r = b_.rows_[i];
                const float h = rowH(r);
                if (y + h >= b.y && y <= b.bottom()) {
                    if (!r.entry) {
                        std::string head = r.heading;
                        for (char& ch : head)
                            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                        c.text({b.x + 2, y + 6, b.w - 4, h - 6}, head, ui::Font::UiBold, 9.5f,
                               t.color("ink-faint"), ui::kLeft | ui::kMiddle);
                    } else {
                        const Entry& e = b_.entries_[*r.entry];
                        const ui::Rect box{b.x - 4, y + 1, b.w + 8, h - 2};
                        if (selected_ == i)
                            c.fillRound(box, 4, t.color("selection"));
                        else if (hover_ == i)
                            c.fillRound(box, 4, t.color("raised"));
                        c.text({b.x + 4, y + 3, b.w - 8, 16}, e.name, ui::Font::UiBold, 12,
                               t.color("ink"));
                        c.text({b.x + 4, y + 19, b.w - 8, 14}, e.description, ui::Font::Ui, 10.5f,
                               t.color("ink-quiet"));
                    }
                }
                y += h;
            }
            if (b_.rows_.empty())
                c.text({b.x, b.y + 8, b.w, 16},
                       !b_.patches_                 ? "No module matches"
                       : b_.search_->text().empty() ? "No patches"
                                                    : "No patch matches",
                       ui::Font::Ui, 11, t.color("ink-faint"), ui::kCenter | ui::kMiddle);
            c.restore();
        }
        bool onPointer(const ui::PointerEvent& e) override {
            using T = ui::PointerEvent::Type;
            const ui::Rect b = bounds();
            if (e.type == T::Wheel) {
                float total = 0;
                for (const auto& r : b_.rows_) total += rowH(r);
                scroll_ = std::clamp(scroll_ - e.wheelY * 36, 0.0f, std::max(0.0f, total - b.h));
                hover_.reset();
                b_.hideCard();
                invalidate();
                return true;
            }
            if (e.type == T::Leave) {
                hover_.reset();
                b_.hideCard();
                invalidate();
                return true;
            }
            if (e.type == T::Move || e.type == T::Enter) {
                if (press_) {
                    const bool out = !b.contains(e.pos);
                    if (std::hypot(e.pos.x - pressAt_.x, e.pos.y - pressAt_.y) > 4)
                        dragging_ = true;
                    if (dragging_ && b_.hooks_.dragOver)
                        b_.hooks_.dragOver(b_.entries_[*b_.rows_[*press_].entry].name,
                                           out ? std::optional(e.pos) : std::nullopt);
                    return true;
                }
                const auto row = rowAt(e.pos);
                if (row != hover_) {
                    hover_ = row;
                    if (row)
                        b_.showCard(*row, rowTop(*row));
                    else
                        b_.hideCard();
                    invalidate();
                }
                return true;
            }
            if (e.type == T::Down && e.button == 3) {
                const auto row = rowAt(e.pos);
                if (!row || !b_.rows_[*row].entry) return true;
                selected_ = row;
                invalidate();
                if (b_.hooks_.menu) b_.hooks_.menu(b_.entries_[*b_.rows_[*row].entry].id, e.pos);
                return true;
            }
            if (e.type == T::Down && e.button == 1) {
                const auto row = rowAt(e.pos);
                if (!row || !b_.rows_[*row].entry) return true;
                selected_ = row;
                invalidate();
                if (e.clicks >= 2) {
                    activate();
                    return true;
                }
                press_ = row;
                pressAt_ = e.pos;
                dragging_ = false;
                b_.hideCard();
                return true;
            }
            if (e.type == T::Up || e.type == T::Cancel) {
                const auto row = std::exchange(press_, std::nullopt);
                const bool dropped =
                    std::exchange(dragging_, false) && row && e.type == T::Up && !b.contains(e.pos);
                if (b_.hooks_.dragOver) b_.hooks_.dragOver({}, std::nullopt);
                if (dropped && b_.hooks_.drop)
                    b_.hooks_.drop(b_.entries_[*b_.rows_[*row].entry].id, e.pos);
                return true;
            }
            return false;
        }

      private:
        static float rowH(const Row& r) { return r.entry ? 36.0f : 26.0f; }

      public:
        float rowTop(std::size_t row) const {
            float y = bounds().y - scroll_;
            for (std::size_t i = 0; i < row && i < b_.rows_.size(); ++i) y += rowH(b_.rows_[i]);
            return y;
        }

      private:
        std::optional<std::size_t> rowAt(ui::Point p) const {
            float y = bounds().y - scroll_;
            for (std::size_t i = 0; i < b_.rows_.size(); ++i) {
                const float h = rowH(b_.rows_[i]);
                if (p.y >= y && p.y < y + h) return i;
                y += h;
            }
            return std::nullopt;
        }
        RackBrowser& b_;
        float scroll_ = 0;
        std::optional<std::size_t> selected_, hover_, press_;
        ui::Point pressAt_;
        bool dragging_ = false;
    };

    Hooks hooks_;
    std::vector<Entry> entries_;
    std::vector<Row> rows_;
    std::vector<std::string> allTags_;     // alphabetical
    std::vector<std::string> commonTags_;  // the most used first
    bool tagsOpen_ = false;
    std::optional<std::string> tag_;
    bool patches_ = false;
    ui::Root::OverlayHandle card_;
    Card* cardNode_ = nullptr;
    Views* views_ = nullptr;
    ui::TextField* search_ = nullptr;
    Tags* tags_ = nullptr;
    List* list_ = nullptr;
};

namespace {

// The bar under the space: what the rack holds, or what went wrong.
class StatusBar : public ui::Node {
  public:
    StatusBar() : Node("rack.statusbar") {
        layout.dir = ui::Layout::Dir::Row;
        layout.height = ui::SizeRule::fixed(kStatusH);
        layout.padding = ui::Insets::xy(10, 0);
        layout.align = ui::Layout::Align::Center;
    }
    void paint(ui::Canvas& c) override {
        c.fillRect(bounds(), c.theme().color("panel"));
        c.fillRect({bounds().x, bounds().y, bounds().w, 1}, c.theme().color("rule"));
    }
};

class Header : public ui::Node {
  public:
    Header() : Node("rack.header") {
        layout.dir = ui::Layout::Dir::Row;
        layout.height = ui::SizeRule::fixed(kHeaderH);
        layout.align = ui::Layout::Align::Center;
        layout.padding = {6, 0, 8, 0};
        layout.gap = 4;
    }
    void paint(ui::Canvas& c) override {
        c.fillRect(bounds(), c.theme().color("panel"));
        c.fillRect({bounds().x, bounds().bottom() - 1, bounds().w, 1}, c.theme().color("rule"));
    }
};

}  // namespace

// --- the window ----------------------------------------------------------------------

RackWindow::RackWindow(FloatingPanel& panel, AppActions& actions, cmd::Dispatcher& dispatcher,
                       engine::EngineApp& app, UiState& ui)
    : panel_(panel),
      actions_(actions),
      dispatcher_(dispatcher),
      app_(app),
      ui_(ui),
      watchers_(app),
      pluginParams_(app) {
    actions_.addUi("ui.rack", "View: racks", [this] {
        if (isOpen())
            close();
        else
            open();
    });
    actions_.addUiFor("ui.rack.show", "Track: show rack", "track",
                      [this](const json::Value& v) -> Status {
                          // The first rack homed on the track (or the selected one).
                          std::string text = v.isString() ? v.asString() : std::string();
                          if (text.empty() && !ui_.selectedTracks.empty())
                              text = ui_.selectedTracks.front();
                          const auto track = model::Id::parse(text);
                          if (!track)
                              return Error("gui.rack", "no track given and nothing is selected",
                                           "/track", "select a channel first");
                          for (const auto& e : app_.head().racks)
                              if (e.value->track == *track) {
                                  open(e.id);
                                  return {};
                              }
                          return Error("gui.rack", "the track has no rack", "/track",
                                       "add one with Track: add rack");
                      });
    actions_.addUiFor(
        "ui.rack.add", "Track: add rack", "track", [this](const json::Value& v) -> Status {
            const auto track = v.isString() ? model::Id::parse(v.asString()) : std::nullopt;
            return addRack(track);
        });
}

RackWindow::~RackWindow() { close(); }

Result<json::Value> RackWindow::exec(const std::string& command, json::Value args,
                                     const std::string& gesture) {
    cmd::Origin origin = kOrigin;
    origin.gesture = gesture;
    auto r = dispatcher_.execute({command, std::move(args), origin});
    if (!r) report(r.error());
    return r;
}

void RackWindow::report(const Error& e) {
    error_ = e.message + (e.hint.empty() ? "" : " (" + e.hint + ")");
    if (status_) status_->setText(status());
    if (!isOpen() && actions_.onError) actions_.onError("rack: " + error_);
}

std::vector<std::string> RackWindow::tagChips() const {
    return browser_ ? browser_->tagChips() : std::vector<std::string>{};
}

std::optional<ui::Point> RackWindow::tagChipAt(const std::string& label) const {
    return browser_ ? browser_->tagChipAt(label) : std::nullopt;
}

std::vector<std::string> RackWindow::browserShows() const {
    return browser_ ? browser_->shown() : std::vector<std::string>{};
}

void RackWindow::showPatches(bool patches) {
    if (!browser_) return;
    if (!browserVisible_) {
        browserVisible_ = true;
        if (browserColumn_) {
            browserColumn_->visible = true;
            browserColumn_->relayout();
        }
    }
    browser_->setPatches(patches);
    browser_->focusSearch();
}

bool RackWindow::patchesShown() const { return browser_ && browser_->patches(); }

std::optional<ui::Point> RackWindow::browserViewAt(bool patches) const {
    return browser_ ? std::optional(browser_->viewAt(patches)) : std::nullopt;
}

std::optional<ui::Point> RackWindow::browserRowAt(const std::string& id) const {
    return browser_ ? browser_->rowAt(id) : std::nullopt;
}

std::string RackWindow::status() const {
    if (!error_.empty()) return error_;
    if (!rack_ || !space_) return "No racks";
    const auto& s = space_->scene();
    std::string out =
        model::rackLabel(app_.head(), *rack_) + "  ·  " + std::to_string(s.modules.size()) +
        (s.modules.size() == 1 ? " module" : " modules") + "  ·  " +
        std::to_string(s.cables.size()) + (s.cables.size() == 1 ? " cable" : " cables");
    if (!space_->selection().empty())
        out += "  ·  " + std::to_string(space_->selection().size()) + " selected";
#if defined(__APPLE__)
    out += "  ·  Wheel: zoom; Cmd+wheel: scroll; Shift+wheel: across";
#else
    out += "  ·  Wheel: zoom; Ctrl+wheel: scroll; Shift+wheel: across";
#endif
    if (capture_) out += "  ·  " + capture_->progress;
    if (waitsForPlay()) out += "  ·  Stopped: press play to start the patch";
    if (!notice_.empty() && app_.head().version == noticeVersion_) out += "  ·  " + notice_;
    return out;
}

void RackWindow::open(std::optional<model::Id> rack) {
    if (rack) rack_ = rack;
    if (isOpen()) {
        show(rack_);
        panel_.raise();
        return;
    }
    if (auto ready = panel_.ensure(); !ready) {
        if (actions_.onError) actions_.onError(ready.error().message);
        return;
    }
    panel_.setTitle("Racks");
    presetsRead_ = false;  // saved elsewhere meanwhile (an agent's rack.preset.save)
    auto content = std::make_unique<ui::Node>("rack");
    content->layout.dir = ui::Layout::Dir::Column;
    auto& header = content->emplace<Header>();
    browserToggle_ = &header.emplace<ui::Button>("rack.browser.toggle", "Browser", [this] {
        browserVisible_ = !browserVisible_;
        if (browser_ && !browserVisible_) browser_->dismissCard();
        if (browserColumn_) {
            browserColumn_->visible = browserVisible_;
            browserColumn_->relayout();
        }
    });
    RackTabs::Hooks th;
    th.select = [this](model::Id id) { show(id); };
    th.rename = [this](model::Id id) { renameRack(id); };
    th.menu = [this](model::Id id, ui::Point at) { tabMenu(id, at); };
    th.add = [this] { (void)addRack(); };
    tabs_ = &header.emplace<RackTabs>(std::move(th));
    zoom_ = &header.emplace<ui::Button>("rack.zoom", "100 %", [this] {
        if (!space_) return;
        if (std::fabs(space_->view().zoom - 1) < 1e-6)
            space_->frameAll();
        else {
            const ui::Rect b = space_->bounds();
            space_->zoomAt({b.x + b.w / 2, b.y + b.h / 2}, 1 / space_->view().zoom);
        }
    });
    auto& body = content->emplace<ui::Node>("rack.body");
    body.layout.dir = ui::Layout::Dir::Row;
    RackBrowser::Hooks bh;
    bh.add = [this](const std::string& type) { (void)addModule(type); };
    bh.menu = [this](const std::string& type, ui::Point at) { browserMenu(type, at); };
    bh.dragOver = [this](const std::string& name, std::optional<ui::Point> at) {
        if (space_)
            space_->dragTypeOver(name, at && space_->bounds().contains(*at) ? at : std::nullopt);
    };
    bh.drop = [this](const std::string& type, ui::Point at) {
        if (space_ && space_->bounds().contains(at)) (void)addModule(type, space_->toSpace(at));
    };
    browser_ = &body.emplace<RackBrowser>(std::move(bh));
    browserColumn_ = browser_;
    browserColumn_->visible = browserVisible_;
    RackSpace::Hooks sh;
    sh.move = [this](const std::vector<RackSpace::Move>& moves) { move(moves); };
    sh.open = [this](model::Id module) {
        const model::RackModule* m = app_.head().modules.find(module);
        if (!m || !m->fx) return;
        if (!openEditor) return;
        if (auto r = openEditor(*m->fx); !r) report(r.error());
    };
    sh.displayEdit = [this](model::Id module, const std::vector<ModuleFace::Edit>& edits,
                            const std::string& gesture) {
        const auto* m = space_ ? space_->scene().module(module) : nullptr;
        if (!m || !m->type) return;
        json::Value params = json::Value::object();
        for (const auto& e : edits)
            if (e.param >= 0 && static_cast<std::size_t>(e.param) < m->type->params().size()) {
                const auto& p = m->type->params()[static_cast<std::size_t>(e.param)];
                params[p.key] = racks::paramJson(p, e.value);
            }
        json::Value args = json::Value::object();
        args["module"] = module.str();
        args["params"] = std::move(params);
        (void)exec("module.set", std::move(args), gesture);
    };
    sh.moduleMenu = [this](model::Id module, ui::Point at) { moduleMenu(module, at); };
    sh.spaceMenu = [this](ui::Point at, ui::Point sp) { spaceMenu(at, sp); };
    sh.param = [this](model::Id module, int index, double value, ui::Gesture g) {
        param(module, index, value, g);
    };
    sh.paramMenu = [this](model::Id module, int index, ui::Point at) {
        paramMenu(module, index, at);
    };
    sh.connect = [this](const model::CableEnd& from, const model::CableEnd& to,
                        std::optional<model::Id> replace) { connect(from, to, replace); };
    sh.pickParam = [this](const model::CableEnd& from, model::Id module,
                          std::optional<model::Id> replace,
                          ui::Point at) { pickParam(from, module, replace, at); };
    sh.cableMenu = [this](model::Id cable, ui::Point at) { cableMenu(cable, at); };
    sh.quickAdd = [this](ui::Point at, ui::Point sp, const std::string& text,
                         std::optional<model::CableEnd> from,
                         bool fromOutput) { quickAdd(at, sp, text, from, fromOutput); };
    sh.copy = [this] { copySelected(); };
    sh.cut = [this] {
        if (copySelected()) deleteSelected();
    };
    sh.duplicate = [this] { duplicateSelected(); };
    sh.paste = [this](ui::Point sp) {
        if (auto r = paste(sp); !r) report(r.error());
    };
    sh.disconnect = [this](model::Id cable) {
        json::Value args = json::Value::object();
        args["cable"] = cable.str();
        (void)exec("cable.remove", std::move(args));
    };
    sh.amount = [this](model::Id cable, double amount, ui::Gesture g) {
        if (g == ui::Gesture::Begin) ++gesture_;
        if (g != ui::Gesture::Move) return;
        json::Value args = json::Value::object();
        args["cable"] = cable.str();
        args["amount"] = amount;
        (void)exec("cable.set", std::move(args),
                   "rack.amount:" + cable.str() + ":" + std::to_string(gesture_));
    };
    sh.selectionChanged = [this](const std::set<model::Id>&) {
        if (status_) status_->setText(status());
    };
    sh.viewChanged = [this](const RackSpace::View& v) {
        if (rack_) views_[*rack_] = v;
        if (zoom_)
            zoom_->setText(std::to_string(static_cast<int>(std::lround(v.zoom * 100))) + " %");
        // What is in view changed: watch it from the next tick.
        nextTelemetry_ = std::chrono::steady_clock::now();
    };
    sh.deleteSelected = [this] { deleteSelected(); };
    sh.start = [this](bool patch) { showPatches(patch); };
    sh.spaceTap = [this] {
        if (spaceTap) spaceTap();
    };
    space_ = &body.emplace<RackSpace>(std::move(sh));
    auto& bar = content->emplace<StatusBar>();
    status_ = &bar.emplace<ui::Label>("rack.status", "", ui::Font::Ui, "small", "ink-quiet");
    status_->layout.width = ui::SizeRule::fraction();
    stop_ = &bar.emplace<ui::Button>("rack.capture.stop", "Stop", [this] { stopCapture(); });
    stop_->layout.height = ui::SizeRule::fixed(18);
    panel_.open(std::move(content), [this] {
        watch(false);
        space_ = nullptr;
        tabs_ = nullptr;
        browser_ = nullptr;
        browserColumn_ = nullptr;
        status_ = nullptr;
        stop_ = nullptr;
        zoom_ = nullptr;
        browserToggle_ = nullptr;
        nextTelemetry_.reset();
        plugins_.reset();
    });
    version_ = projectGeneration_ = ~std::uint64_t{0};
    if (auto r = reload(); !r) report(r.error());
    root().focus(space_);
    showCapture();
}

namespace {

// What can be added: Plugin… first, then the types by origin, as the
// Modules view lists them; then the rack patches, the user's before the
// factory's, as the Patches view does.
std::vector<RackBrowser::Entry> entriesOf(const rack::Catalog& catalog,
                                          const json::Value& presets) {
    std::vector<RackBrowser::Entry> entries;
    auto tagsLine = [](const std::vector<std::string>& tags) {
        std::string out;
        for (const auto& t : tags) out += (out.empty() ? "" : " \u00b7 ") + t;
        return out;
    };
    // Plug-ins are modules too: picked from the plug-in browser.
    const auto& plugin = racks::pluginType()->info();
    entries.push_back({plugin.id, "Plugin\u2026",
                       "A CLAP or VST3 plug-in; a click opens its editor.", plugin.tags,
                       rack::Origin::Host, false, false, tagsLine(plugin.tags)});
    for (const rack::Catalog::Entry* e : catalog.list()) {
        const auto& info = e->type->info();
        if (info.id == plugin.id) continue;
        entries.push_back({info.id, info.name, info.description, info.tags, e->type->origin(),
                           false, false, tagsLine(info.tags)});
    }
    // Rack patches: whole patches, loaded as a module type is added.
    std::vector<RackBrowser::Entry> saved, factory;
    if (const json::Value* list = presets.find("presets"); list && list->isArray())
        for (const auto& p : list->asArray()) {
            RackBrowser::Entry e;
            e.name = str(p, "name");
            e.id = std::string(kPresetPrefix) + e.name;
            e.preset = true;
            const json::Value* f = p.find("factory");
            e.factory = f && f->isBool() && f->asBool();
            // "Factory patch · 6 modules, 9 cables: ADSR, Filter, …"
            auto count = [&](const char* key) {
                const json::Value* n = p.find(key);
                return n && n->isNumber() ? static_cast<long>(n->asInt()) : 0L;
            };
            const long modules = count("modules"), cables = count("cables");
            std::string contents =
                std::to_string(modules) + (modules == 1 ? " module" : " modules");
            if (cables > 0)
                contents += ", " + std::to_string(cables) + (cables == 1 ? " cable" : " cables");
            if (const json::Value* types = p.find("types"); types && types->isArray()) {
                std::string names;
                for (const auto& t : types->asArray()) {
                    const rack::Catalog::Entry* ce = catalog.find(t.asString());
                    names +=
                        (names.empty() ? ": " : ", ") + (ce ? ce->type->info().name : t.asString());
                }
                contents += names;
            }
            e.description = str(p, "description");
            if (e.description.empty()) e.description = contents;
            e.detail =
                std::string(e.factory ? "Factory patch" : "Your patch") + " \u00b7 " + contents;
            (e.factory ? factory : saved).push_back(std::move(e));
        }
    for (auto* group : {&saved, &factory})
        entries.insert(entries.end(), std::make_move_iterator(group->begin()),
                       std::make_move_iterator(group->end()));
    return entries;
}

}  // namespace

Status RackWindow::reload() {
    const model::Session& s = app_.head();
    catalog_ = racks::catalog(app_);
    projectGeneration_ = app_.projectGeneration();
    version_ = s.version;
    if (rack_ && !s.racks.find(*rack_)) rack_.reset();
    if (!rack_ && !s.racks.empty()) rack_ = s.racks.begin()->id;
    refreshTabs();
    if (browser_) {
        if (!presetsRead_) readPresets();
        browser_->setEntries(entriesOf(*catalog_, presets_));
    }
    if (!space_) return {};
    const bool other = !(rack_ ? space_->scene().rack == *rack_ : false);
    // Whether a plug-in plays notes: from the scan, which adding a plugin
    // module (or loading one) has made.
    auto instrument = [this](const model::PluginRef& ref) {
        auto list = app_.plugins();
        if (!list) return false;
        for (const auto& p : *list)
            if (p.id == ref.id) return p.instrument;
        return false;
    };
    bool plugins = false;
    if (rack_)
        for (const auto& e : s.modules)
            plugins = plugins || (e.value->rack == *rack_ && e.value->fx);
    RackScene scene = rack_ ? RackScene::build(s, *rack_, *catalog_,
                                               plugins ? std::function(instrument) : nullptr)
                            : RackScene{};
    // Cables that close a loop carry the previous frame; they are drawn so.
    if (rack_ && !scene.cables.empty()) {
        json::Value args = json::Value::object();
        args["rack"] = rack_->str();
        if (auto got = dispatcher_.execute({"rack.get", std::move(args), kOrigin}))
            if (const json::Value* cables = got->find("cables"); cables && cables->isArray())
                for (const auto& c : cables->asArray()) {
                    const json::Value* fb = c.find("feedback");
                    if (!fb || !fb->isBool() || !fb->asBool()) continue;
                    const auto id = model::Id::parse(str(c, "cable"));
                    for (auto& k : scene.cables)
                        if (id && k.id == *id) k.feedback = true;
                }
    }
    // A plug-in parameter's socket shows the parameter's name.
    for (auto& m : scene.modules)
        if (m.fx && !m.mods.empty()) {
            for (const std::string& key : m.mods) m.modNames[key] = pluginParams_.name(*m.fx, key);
        }
    space_->setScene(std::move(scene));
    space_->setEmptyText(s.racks.empty() ? "No racks yet. + adds one on the selected channel."
                                         : "");
    space_->setStartHint(rackEmpty());
    if (other && rack_) {
        if (const auto v = views_.find(*rack_); v != views_.end())
            space_->setView(v->second);
        else
            space_->frameAllWhenLaidOut();
    }
    nextTelemetry_ = std::chrono::steady_clock::now();
    if (status_) status_->setText(status());
    return {};
}

void RackWindow::refreshTabs() {
    if (!tabs_) return;
    const model::Session& s = app_.head();
    std::vector<RackTabs::Tab> tabs;
    for (const auto& e : s.racks) {
        RackTabs::Tab t;
        t.rack = e.id;
        t.label = model::rackLabel(s, e.id);
        if (const model::Track* tr = s.tracks.find(e.value->track))
            t.color = root().theme().trackColor(tr->color);
        tabs.push_back(std::move(t));
    }
    tabs_->set(std::move(tabs), rack_);
    panel_.setTitle(rack_ ? "Racks — " + model::rackLabel(s, *rack_) : std::string("Racks"));
}

void RackWindow::show(std::optional<model::Id> rack) {
    if (rack && !app_.head().racks.find(*rack)) return;
    if (rack == rack_ && space_ && space_->scene().rack == rack_.value_or(model::Id{})) return;
    watch(false);
    rack_ = rack;
    error_.clear();
    version_ = ~std::uint64_t{0};
    (void)reload();
}

void RackWindow::sync() {
    if (!isOpen()) return;
    if (app_.projectGeneration() != projectGeneration_ || app_.head().version != version_) {
        error_.clear();
        (void)reload();
    }
}

void RackWindow::watch(bool on) {
    if (on) return;
    watchers_.set({});
}

bool RackWindow::tick(std::chrono::steady_clock::time_point now) {
    if (plugins_ && plugins_->isOpen()) plugins_->tick();
    const bool captured = pollCapture();  // with the window closed too
    bool shown = captured;
    if (isOpen() && waitsForPlay() != waiting_) {  // played or stopped, from anywhere
        waiting_ = !waiting_;
        if (status_) status_->setText(status());
        shown = true;
    }
    if (!isOpen() || !nextTelemetry_ || now < *nextTelemetry_) return shown;
    nextTelemetry_ = now + kTelemetryInterval;
    bool changed = false;
    const ui::Rect seen = space_->visibleSpace();
    // Modules in view are watched. Out of view, the sources of cables that
    // cross it are measured, for their cables' glow (no telemetry; the
    // engine measures while the transport plays).
    const std::vector<model::Id> sources = space_->cableSources(seen);
    const engine::Engine* engine = app_.engine();
    const bool playing = engine && engine->running() && engine->transport().playing;
    std::vector<model::Id> watching;
    bool measuring = false;
    for (auto& m : space_->scene().modules) {
        rack::Instance* in = racks::instance(app_, m.id);
        const bool visible = !seen.intersect(m.rect()).empty();
        const bool source = !visible && playing && in && m.type &&
                            std::find(sources.begin(), sources.end(), m.id) != sources.end();
        if (source) {
            std::vector<float> levels;
            for (int k = 0; k < static_cast<int>(m.type->outputs().size()); ++k)
                levels.push_back(app_.moduleOutputLevel(m.id, m.type->output(k).key));
            if (!m.measured || levels != m.levels) changed = true;
            m.levels = std::move(levels);
            m.measured = measuring = true;
        } else if (m.measured) {
            m.measured = false;
            m.levels.clear();
            changed = true;
        }
        if (!in || !visible || !m.type) {
            if (m.live) {
                m.live = false;
                changed = true;
            }
            continue;
        }
        in->watch(true);
        watching.push_back(m.id);
        if (in->readTelemetry(m.telemetry)) {
            m.live = true;
            changed = true;
        }
    }
    const bool none = watching.empty();
    watchers_.set(std::move(watching));  // and off for those no view reads
    if (none && !measuring) nextTelemetry_ = now + std::chrono::milliseconds(250);
    if (changed) space_->telemetryChanged();
    return changed || shown;
}

std::optional<std::chrono::steady_clock::time_point> RackWindow::nextDeadline() const {
    auto next = isOpen() ? nextTelemetry_ : std::nullopt;
    if (capture_) {  // the job is followed, with the window closed too
        const auto poll = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        if (!next || poll < *next) next = poll;
    }
    return next;
}

json::Value RackWindow::saveLayout() const {
    json::Value out = json::Value::object();
    if (rack_) out["rack"] = rack_->str();
    out["browser"] = browserVisible_;
    json::Value views = json::Value::object();
    for (const auto& [id, v] : views_) {
        if (!app_.head().racks.find(id)) continue;
        json::Value one = json::Value::object();
        one["x"] = std::round(v.x * 10) / 10;
        one["y"] = std::round(v.y * 10) / 10;
        one["zoom"] = std::round(v.zoom * 1000) / 1000;
        views[id.str()] = std::move(one);
    }
    out["views"] = std::move(views);
    return out;
}

void RackWindow::loadLayout(const json::Value& layout) {
    views_.clear();
    if (!layout.isObject()) return;
    if (const auto* r = layout.find("rack"); r && r->isString())
        if (auto id = model::Id::parse(r->asString()); id && id->kind == model::Kind::Rack)
            rack_ = id;
    if (const auto* b = layout.find("browser"); b && b->isBool()) browserVisible_ = b->asBool();
    if (const auto* views = layout.find("views"); views && views->isObject())
        for (const auto& m : views->asObject()) {
            const auto id = model::Id::parse(m.key);
            if (!id || id->kind != model::Kind::Rack || !m.value.isObject()) continue;
            auto num = [&](const char* k, double def) {
                const auto* v = m.value.find(k);
                return v && v->isNumber() && std::isfinite(v->asDouble()) ? v->asDouble() : def;
            };
            views_[*id] = {num("x", 0), num("y", 0),
                           std::clamp(num("zoom", 1), RackSpace::kMinZoom, RackSpace::kMaxZoom)};
        }
    if (isOpen()) {
        version_ = ~std::uint64_t{0};
        if (browserColumn_) {
            browserColumn_->visible = browserVisible_;
            browserColumn_->relayout();
        }
        if (space_) space_->setScene({});  // so the saved view applies
        (void)reload();
    }
}

// --- edits ---------------------------------------------------------------------------

Status RackWindow::addRack(std::optional<model::Id> track) {
    const model::Session& s = app_.head();
    if (!track && !ui_.selectedTracks.empty())
        if (auto id = model::Id::parse(ui_.selectedTracks.front()))
            if (const model::Track* t = s.tracks.find(*id);
                t && t->type == model::StripType::Channel)
                track = id;
    json::Value args = json::Value::object();
    if (track) args["track"] = track->str();
    auto r = exec("rack.add", std::move(args));
    if (!r) return std::move(r).error();
    const auto id = model::Id::parse(str(*r, "rack"));
    open(id);
    show(id);
    return {};
}

Status RackWindow::addModule(const std::string& type, std::optional<ui::Point> at) {
    if (!rack_) {
        if (auto r = addRack(); !r) return r;
    }
    if (type.starts_with(kPresetPrefix)) return loadPreset(type.substr(kPresetPrefix.size()), at);
    if (type == model::kPluginModule && isOpen()) {  // which plug-in: the browser says
        if (!plugins_)
            plugins_ = std::make_unique<PluginBrowser>(app_, dispatcher_, root(), favouritesFile);
        plugins_->open(
            [this, at](const std::string& id) {
                if (plugins_) plugins_->close();
                json::Value args = json::Value::object();
                args["rack"] = rack_->str();
                args["plugin"] = id;
                std::optional<ui::Point> place = at;
                if (!place && space_) {
                    const ui::Rect v = space_->visibleSpace();
                    place = ui::Point{v.x + v.w / 2 - 80, v.y + v.h / 2 - 60};
                }
                if (place) {
                    args["x"] = std::round(static_cast<double>(place->x));
                    args["y"] = std::round(static_cast<double>(place->y));
                }
                if (auto r = exec("module.add", std::move(args)); r && space_) {
                    sync();
                    if (auto made = model::Id::parse(str(*r, "module"))) space_->select({*made});
                }
            },
            "Add a plug-in to the rack");
        return {};
    }
    json::Value args = json::Value::object();
    args["rack"] = rack_->str();
    args["type"] = type;
    if (!at && space_) {  // the middle of the view
        const ui::Rect v = space_->visibleSpace();
        at = ui::Point{v.x + v.w / 2 - 50, v.y + v.h / 2 - 60};
    }
    if (at) {
        args["x"] = std::round(static_cast<double>(at->x));
        args["y"] = std::round(static_cast<double>(at->y));
    }
    auto r = exec("module.add", std::move(args));
    if (!r) return std::move(r).error();
    sync();
    if (space_)
        if (auto id = model::Id::parse(str(*r, "module"))) space_->select({*id});
    return {};
}

void RackWindow::move(const std::vector<RackSpace::Move>& moves) {
    auto r = dispatcher_.transact(
        moves.size() == 1 ? "Move module" : "Move modules", kOrigin, [&]() -> Result<json::Value> {
            for (const auto& m : moves) {
                json::Value args = json::Value::object();
                args["module"] = m.module.str();
                args["x"] = m.x;
                args["y"] = m.y;
                SUPPLE_TRY(dispatcher_.execute({"module.set", std::move(args), kOrigin}));
            }
            return json::Value::object();
        });
    if (!r) {
        report(r.error());
        version_ = ~std::uint64_t{0};  // back to where the session has them
        sync();
    }
}

void RackWindow::param(model::Id module, int index, double value, ui::Gesture g) {
    if (g == ui::Gesture::Begin) {
        ++gesture_;
        return;
    }
    if (g == ui::Gesture::End) return;
    const auto* m = space_ ? space_->scene().module(module) : nullptr;
    if (!m || !m->type || index < 0 || static_cast<std::size_t>(index) >= m->type->params().size())
        return;
    const auto& p = m->type->params()[static_cast<std::size_t>(index)];
    json::Value params = json::Value::object();
    params[p.key] = racks::paramJson(p, value);
    json::Value args = json::Value::object();
    args["module"] = module.str();
    args["params"] = std::move(params);
    (void)exec("module.set", std::move(args),
               g == ui::Gesture::Move
                   ? "rack.param:" + module.str() + ":" + p.key + ":" + std::to_string(gesture_)
                   : std::string());
}

void RackWindow::connect(const model::CableEnd& from, const model::CableEnd& to,
                         std::optional<model::Id> replace, std::optional<double> amount) {
    if (replace)
        if (const model::Cable* old = app_.head().cables.find(*replace);
            old && old->from == from && old->to == to)
            return;  // dropped back where it was
    auto add = [&]() -> Result<json::Value> {
        json::Value args = json::Value::object();
        args["from"] = from.str();
        args["to"] = to.str();
        if (amount) args["amount"] = *amount;
        return dispatcher_.execute({"cable.add", std::move(args), kOrigin});
    };
    Result<json::Value> r = json::Value();
    if (replace)
        r = dispatcher_.transact("Move cable", kOrigin, [&]() -> Result<json::Value> {
            json::Value args = json::Value::object();
            args["cable"] = replace->str();
            SUPPLE_TRY(dispatcher_.execute({"cable.remove", std::move(args), kOrigin}));
            return add();
        });
    else
        r = add();
    if (!r) report(r.error());
}

namespace {

// A search field over a list, in a popup: the plug-in parameters a cable can
// modulate, or the modules to add at the pointer. Enter or a double click
// chooses; Escape closes. Without rows it says why (`note`).
struct PickRow {
    std::string label, detail, key;
    std::string search;  // more words that match (an id, tags, a description)
};

class PickList : public ui::Node {
  public:
    PickList(ui::Root& root, std::string id, const std::string& title, std::vector<PickRow> rows,
             const std::string& note, const std::string& text, const std::string& placeholder,
             float width, std::function<void(const PickRow&)> chosen)
        : Node(id), root_(root), rows_(std::move(rows)), chosen_(std::move(chosen)) {
        layout.dir = ui::Layout::Dir::Column;
        layout.width = ui::SizeRule::fixed(width);
        layout.padding = ui::Insets::all(10);
        layout.gap = 6;
        auto& heading = emplace<ui::Label>(id + ".title", title, ui::Font::UiBold, "small");
        heading.layout.height = ui::SizeRule::fixed(18);
        if (rows_.empty()) {
            layout.height = ui::SizeRule::content();
            auto& line = emplace<ui::Label>(id + ".note", note, ui::Font::Ui, "small", "ink-quiet");
            line.layout.height = ui::SizeRule::fixed(20);
            return;
        }
        layout.height = ui::SizeRule::fixed(
            std::min(380.0f, 116.0f + 26.0f * static_cast<float>(rows_.size())));
        search_ = &emplace<ui::TextField>(id + ".search", text, placeholder);
        list_ = &emplace<ui::List>(id + ".list");
        list_->layout.height = ui::SizeRule::fraction();
        list_->rowHeight = 26;
        auto& hint = emplace<ui::Label>(id + ".hint", "Enter chooses · Esc closes", ui::Font::Ui,
                                        "caption", "ink-faint");
        hint.layout.height = ui::SizeRule::fixed(16);
        search_->onChange = [this](const std::string& q) { filter(q); };
        search_->onSubmit = [this](const std::string&) { choose(); };
        search_->onCancel = [this] { root_.closePopup(); };
        search_->onOtherKey = [this](const ui::KeyEvent& e) { return list_->onKey(e); };
        list_->onActivate = [this](std::size_t) { choose(); };
        list_->paintRow = [this](ui::Canvas& c, std::size_t i, const ui::Rect& r, bool) {
            const ui::Theme& t = c.theme();
            const PickRow& row = rows_[matches_[i]];
            const float labelW = c.textWidth(row.label, ui::Font::Ui, t.type("small"));
            c.text({r.x + 8, r.y, r.w - 16, r.h}, row.label, ui::Font::Ui, t.type("small"),
                   t.color("ink"));
            if (!row.detail.empty() && labelW + 30 < r.w) {
                c.save();
                c.clip({r.x + 18 + labelW, r.y, r.w - 26 - labelW, r.h});
                c.text({r.x + 18 + labelW, r.y, r.w - 26 - labelW, r.h}, row.detail, ui::Font::Ui,
                       t.type("caption"), t.color("ink-quiet"));
                c.restore();
            }
        };
        filter(text);
    }
    ui::TextField* search() const { return search_; }
    void paint(ui::Canvas&) override {}
    bool onText(std::string_view typed) override {
        if (!search_) return false;
        root_.focus(search_);
        return search_->onText(typed);
    }
    std::string valueText() const override {
        std::string out;
        for (std::size_t i : matches_) out += (out.empty() ? "" : " | ") + rows_[i].label;
        return out;
    }

  private:
    // Names that start with the text first, then a word of the name, then
    // the name containing it, then the other words.
    void filter(const std::string& q) {
        const std::string want = lower(q);
        std::vector<std::pair<int, std::size_t>> ranked;
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            const std::string name = lower(rows_[i].label);
            int rank = -1;
            if (want.empty() || name.starts_with(want))
                rank = 0;
            else if (name.find(" " + want) != std::string::npos)
                rank = 1;
            else if (name.find(want) != std::string::npos)
                rank = 2;
            else if (lower(rows_[i].detail + " " + rows_[i].search).find(want) != std::string::npos)
                rank = 3;
            if (rank >= 0) ranked.emplace_back(rank, i);
        }
        std::stable_sort(ranked.begin(), ranked.end(),
                         [](const auto& x, const auto& y) { return x.first < y.first; });
        matches_.clear();
        for (const auto& r : ranked) matches_.push_back(r.second);
        list_->setCount(matches_.size());
        list_->select(matches_.empty() ? std::nullopt : std::optional<std::size_t>(0));
        if (!matches_.empty()) list_->scrollTo(0);
    }
    void choose() {
        const auto row = list_->selected();
        if (!row || *row >= matches_.size()) return;
        const PickRow chosen = rows_[matches_[*row]];  // closing destroys the list
        auto run = chosen_;
        root_.closePopup();
        run(chosen);
    }
    ui::Root& root_;
    std::vector<PickRow> rows_;
    std::function<void(const PickRow&)> chosen_;
    std::vector<std::size_t> matches_;
    ui::TextField* search_ = nullptr;
    ui::List* list_ = nullptr;
};

}  // namespace

void RackWindow::pickParam(const model::CableEnd& from, model::Id module,
                           std::optional<model::Id> replace, ui::Point at) {
    const auto* m = space_ ? space_->scene().module(module) : nullptr;
    if (!m || !m->fx) return;
    // A parameter cable moved here keeps its amount.
    std::optional<double> amount;
    if (replace)
        if (const model::Cable* old = app_.head().cables.find(*replace);
            old && old->to.object.kind == model::Kind::Fx)
            amount = old->amount;
    const model::Id fx = *m->fx;
    const racks::PluginParams& params = paramsOf(fx);
    std::vector<PickRow> rows;
    for (const auto& q : params.params)
        if (q.modulatable) rows.push_back({q.name, q.group, q.key, {}});
    auto picker = std::make_unique<PickList>(
        root(), "rack.params", "Modulate " + m->title, std::move(rows), params.note, "",
        "Search parameters…", 300.0f, [this, from, fx, replace, amount](const PickRow& row) {
            connect(from, model::CableEnd{fx, row.key}, replace, amount);
        });
    ui::TextField* search = picker->search();
    root().openPopup(std::move(picker), {at.x, at.y, 0, 0});
    if (search) root().focus(search);
}

void RackWindow::cableMenu(model::Id id, ui::Point at) {
    const model::Session& s = app_.head();
    const model::Cable* cable = s.cables.find(id);
    if (!cable || !space_) return;
    const RackScene& scene = space_->scene();
    // What it reaches, as people say it: "Gain on Test Gain".
    std::string what = cable->to.key, where;
    double scale = 1;  // the amount as shown: a plug-in parameter's in percent
    std::string unit;
    if (cable->to.object.kind == model::Kind::Fx) {
        if (const auto* m = scene.moduleOfFx(cable->to.object)) {
            what = m->modName(cable->to.key);
            where = m->title;
        }
        scale = 100;
        unit = " %";
    } else if (const auto* m = scene.module(cable->to.object)) {
        where = m->title;
        if (m->type) {
            if (const int p = m->type->param(cable->to.key); p >= 0)
                what = m->type->params()[static_cast<std::size_t>(p)].name;
            else if (const int in = m->type->input(cable->to.key); in >= 0)
                what = m->type->input(in).name;
        }
    }
    std::vector<ui::MenuItem> items;
    items.push_back({what + (where.empty() ? "" : " on " + where), "", {}, false});
    if (cable->amount) {
        char text[48];
        std::snprintf(text, sizeof text, scale == 100 ? "%+.0f" : "%+.3g", *cable->amount * scale);
        const std::string shown = text + unit;
        items.push_back({"Amount: " + shown + "…", "", [this, id, at, scale, shown] {
                             openNameField(root(), "rack.cable.amount", shown, {at.x, at.y, 120, 0},
                                           [this, id, scale](const std::string& typed) {
                                               char* end = nullptr;
                                               const double v = std::strtod(typed.c_str(), &end);
                                               if (end == typed.c_str() || !std::isfinite(v)) {
                                                   report(Error("cable.amount",
                                                                "“" + typed + "” is not a number"));
                                                   return;
                                               }
                                               json::Value args = json::Value::object();
                                               args["cable"] = id.str();
                                               args["amount"] = v / scale;
                                               (void)exec("cable.set", std::move(args));
                                           });
                         }});
    }
    if (cable->to.object.kind == model::Kind::Fx)
        if (const auto* m = scene.moduleOfFx(cable->to.object))
            items.push_back(
                {"Change parameter…", "", [this, from = cable->from, module = m->id, id, at] {
                     pickParam(from, module, id, at);
                 }});
    items.push_back({"", "", {}, true, true});
    items.push_back({"Remove cable", "", [this, id] {
                         json::Value args = json::Value::object();
                         args["cable"] = id.str();
                         (void)exec("cable.remove", std::move(args));
                     }});
    root().openPopup(
        ui::makeMenu("rack.cable.menu", std::move(items), [this] { root().closePopup(); }),
        {at.x, at.y, 0, 0});
}

void RackWindow::quickAdd(ui::Point at, ui::Point sp, const std::string& text,
                          std::optional<model::CableEnd> from, bool fromOutput) {
    if (!catalog_ || !space_) return;
    if (!rack_) {
        if (auto r = addRack(); !r) return report(r.error());
    }
    if (!presetsRead_) readPresets();
    std::vector<PickRow> rows;
    for (const auto& e : entriesOf(*catalog_, presets_)) {
        // A cable needs a module it can plug into now: no plug-in browser, no preset.
        if (from && (e.preset || e.id == model::kPluginModule)) continue;
        std::string tags;
        for (const auto& t : e.tags) tags += t + " ";
        rows.push_back({e.name, e.description, e.id, e.id + " " + tags});
    }
    const std::string title = !from        ? "Add a module"
                              : fromOutput ? "Add a module for this cable"
                                           : "Add a module to feed this input";
    auto list = std::make_unique<PickList>(
        root(), "rack.add", title, std::move(rows), "Nothing to add.", text,
        "Type a module’s name…", 380.0f, [this, sp, from, fromOutput](const PickRow& row) {
            if (auto r = addWired(row.key, sp, from, fromOutput); !r) report(r.error());
        });
    ui::TextField* search = list->search();
    root().openPopup(std::move(list), {at.x, at.y, 0, 0});
    if (search) {
        root().focus(search);
        if (!text.empty()) search->setText(text);  // the letter typed, the cursor after it
    }
}

Status RackWindow::addWired(const std::string& type, ui::Point sp,
                            std::optional<model::CableEnd> from, bool fromOutput) {
    const rack::Catalog::Entry* entry = catalog_ ? catalog_->find(type) : nullptr;
    if (!entry || !from) {
        // Its top centred at the point, the title under the pointer.
        const double w = entry ? entry->type->layout().width : 0;
        return addModule(
            type, ui::Point{static_cast<float>(static_cast<double>(sp.x) - w / 2), sp.y - 12});
    }
    const rack::ModuleType& t = *entry->type;
    // The port the cable takes: the first of its signal, else the first.
    const Signal want = space_ ? space_->scene().signalOf(*from, fromOutput) : Signal::Audio;
    std::string key;
    const int count = static_cast<int>(fromOutput ? t.inputs().size() : t.outputs().size());
    for (int k = 0; k < count && key.empty(); ++k) {
        const auto& port = fromOutput ? t.input(k) : t.output(k);
        if (port.signal == want) key = port.key;
    }
    if (key.empty() && count > 0) key = (fromOutput ? t.input(0) : t.output(0)).key;
    // The module sits beside the pointer, on the side the cable comes from.
    const double w = t.layout().width;
    const double px = static_cast<double>(sp.x), py = static_cast<double>(sp.y);
    const double x = fromOutput ? px - 12 : px - w + 12;
    json::Value made;
    auto r = dispatcher_.transact("Add module", kOrigin, [&]() -> Result<json::Value> {
        json::Value args = json::Value::object();
        args["rack"] = rack_->str();
        args["type"] = type;
        args["x"] = std::round(x);
        args["y"] = std::round(py - 20);
        auto added = dispatcher_.execute({"module.add", std::move(args), kOrigin});
        if (!added) return added;
        made = *added;
        if (key.empty()) return made;
        model::CableEnd end{*model::Id::parse(str(made, "module")), key};
        json::Value cable = json::Value::object();
        cable["from"] = (fromOutput ? *from : end).str();
        cable["to"] = (fromOutput ? end : *from).str();
        SUPPLE_TRY(dispatcher_.execute({"cable.add", std::move(cable), kOrigin}));
        return made;
    });
    if (!r) return std::move(r).error();
    sync();
    if (space_)
        if (auto id = model::Id::parse(str(made, "module"))) space_->select({*id});
    return {};
}

bool RackWindow::copySelected() {
    if (!space_ || space_->selection().empty()) return false;
    json::Value list = json::Value::array();
    for (model::Id id : space_->selection()) list.push_back(id.str());
    json::Value args = json::Value::object();
    args["modules"] = std::move(list);
    auto r = exec("module.copy", std::move(args));
    if (!r) return false;
    const auto* count = r->find("modules");
    const int n = count && count->isNumber() ? static_cast<int>(count->asDouble()) : 0;
    if (status_)
        status_->setText("Copied " + std::to_string(n) + (n == 1 ? " module" : " modules"));
    return true;
}

void RackWindow::duplicateSelected() {
    if (!space_ || space_->selection().empty()) return;
    json::Value args = json::Value::object();
    json::Value list = json::Value::array();
    for (model::Id id : space_->selection()) list.push_back(id.str());
    args["modules"] = std::move(list);
    auto r = exec("module.duplicate", std::move(args));
    if (!r || !space_) return;
    std::set<model::Id> made;
    if (const auto* ids = r->find("modules"); ids && ids->isArray())
        for (const auto& x : ids->asArray())
            if (auto id = x.isString() ? model::Id::parse(x.asString()) : std::nullopt)
                made.insert(*id);
    sync();
    space_->select(made);
    space_->reveal(made);
}

Status RackWindow::paste(ui::Point sp) {
    if (!app_.rackClipboard()) return {};  // module.copy copies one or more
    if (!rack_) SUPPLE_TRY(addRack());
    json::Value args = json::Value::object();
    args["rack"] = rack_->str();
    args["x"] = std::round(static_cast<double>(sp.x));
    args["y"] = std::round(static_cast<double>(sp.y));
    auto r = dispatcher_.execute({"rack.paste", std::move(args), kOrigin});
    if (!r) return std::move(r).error();
    std::set<model::Id> made;
    if (const auto* ids = r->find("modules"); ids && ids->isArray())
        for (const auto& x : ids->asArray())
            if (auto id = x.isString() ? model::Id::parse(x.asString()) : std::nullopt)
                made.insert(*id);
    sync();
    if (space_) {
        space_->select(made);
        space_->reveal(made);
    }
    return {};
}

void RackWindow::deleteSelected() {
    if (!space_ || space_->selection().empty()) return;
    const auto ids = space_->selection();
    auto r = dispatcher_.transact(
        ids.size() == 1 ? "Remove module" : "Remove modules", kOrigin,
        [&]() -> Result<json::Value> {
            for (model::Id id : ids) {
                json::Value args = json::Value::object();
                args["module"] = id.str();
                SUPPLE_TRY(dispatcher_.execute({"module.remove", std::move(args), kOrigin}));
            }
            return json::Value::object();
        });
    if (!r) report(r.error());
}

void RackWindow::tidyUp() {
    if (!rack_ || !space_) return;
    const std::set<model::Id> selected = space_->selection();
    const bool some = selected.size() > 1;
    json::Value args = json::Value::object();
    args["rack"] = rack_->str();
    if (some) {
        json::Value list = json::Value::array();
        for (model::Id id : selected) list.push_back(id.str());
        args["modules"] = std::move(list);
    }
    if (!exec("rack.arrange", std::move(args))) return;
    sync();
    if (some)
        space_->reveal(selected);
    else
        space_->frameAll();
}

namespace {
// The outputs rack.capture reads: the first pitch and gate, and a velocity.
struct NoteOutputs {
    std::string pitch, gate, velocity;
};
std::optional<NoteOutputs> noteOutputs(const rack::ModuleType& type) {
    NoteOutputs o;
    for (int k = 0; k < static_cast<int>(type.outputs().size()); ++k) {
        const auto& port = type.output(k);
        if (port.signal == Signal::Pitch && o.pitch.empty()) o.pitch = port.key;
        if (port.signal == Signal::Gate && o.gate.empty()) o.gate = port.key;
        if (port.key == "velocity") o.velocity = port.key;
    }
    if (o.pitch.empty() || o.gate.empty()) return std::nullopt;
    return o;
}
// The bar the playhead is in (1: the song's start).
int playheadBar(engine::EngineApp& app) {
    const auto t = app.transport("get");
    return app.head().timeMap->barBeat(t ? t->beat : 0.0).bar;
}
}  // namespace

void RackWindow::captureNotes(model::Id module, model::Id track, int bars) {
    const model::Session& s = app_.head();
    const model::RackModule* m = s.modules.find(module);
    const auto* type = m && catalog_ ? catalog_->find(m->type) : nullptr;
    const auto outputs = type ? noteOutputs(*type->type) : std::nullopt;
    if (!outputs || capture_) return;
    const int bar = std::max(1, playheadBar(app_));
    const double from = s.timeMap->beatOfBar(bar);
    const double beats = s.timeMap->beatOfBar(bar + bars) - from;
    json::Value args = json::Value::object();
    args["rack"] = m->rack.str();
    args["pitch"] = module.str() + "." + outputs->pitch;
    args["gate"] = module.str() + "." + outputs->gate;
    if (!outputs->velocity.empty()) args["velocity"] = module.str() + "." + outputs->velocity;
    args["track"] = track.str();
    args["beats"] = beats;
    args["from_beat"] = from;
    args["async"] = true;
    auto r = exec("rack.capture", std::move(args));
    if (!r) return;
    const json::Value* job = r->find("job");
    if (!job || !job->isString()) return;
    capture_ = Capturing{job->asString(), track, "Capturing notes\u2026"};
    notice_.clear();
    showCapture();
}

bool RackWindow::pollCapture() {
    if (!capture_) return false;
    json::Value args = json::Value::object();
    args["job"] = capture_->job;
    auto got = dispatcher_.execute({"job.get", std::move(args), kOrigin});
    const std::string state = got ? str(*got, "status") : std::string();
    if (state == "running") {
        const json::Value* progress = got->find("progress");
        const json::Value* fraction = progress ? progress->find("fraction") : nullptr;
        std::string text = "Capturing notes\u2026";
        if (fraction && fraction->isNumber() && fraction->asDouble() > 0)
            text += " " +
                    std::to_string(static_cast<int>(std::lround(fraction->asDouble() * 100))) +
                    " %";
        if (text == capture_->progress) return false;
        capture_->progress = std::move(text);
        showCapture();
        return true;
    }
    const model::Id track = capture_->track;
    capture_.reset();
    if (!got) {
        report(got.error());
    } else if (state == "done") {
        const json::Value* result = got->find("result");
        const json::Value* count = result ? result->find("notes") : nullptr;
        const int n = count && count->isNumber() ? static_cast<int>(count->asDouble()) : 0;
        notice_ = n == 0 ? "No notes: the gate did not rise in those bars"
                         : "Captured " + std::to_string(n) + (n == 1 ? " note" : " notes") +
                               " to " + model::trackLabel(app_.head(), track);
        noticeVersion_ = app_.head().version;
    } else if (state == "failed") {
        const json::Value* e = got->find("error");
        const std::string code = e ? str(*e, "code") : std::string();
        // The rack, the channel's items or the tempo changed while it
        // rendered: nothing was added.
        if (code == "rack.capture_stale")
            report(Error(code, "The rack or the channel changed while capturing; capture again"));
        else
            report(Error(code, e ? str(*e, "message") : "The capture failed", "",
                         e ? str(*e, "hint") : std::string()));
    } else {
        notice_ = "Capture stopped";
        noticeVersion_ = app_.head().version;
    }
    showCapture();
    return true;
}

void RackWindow::stopCapture() {
    if (!capture_) return;
    json::Value args = json::Value::object();
    args["job"] = capture_->job;
    (void)exec("job.cancel", std::move(args));
    (void)pollCapture();
}

bool RackWindow::waitsForPlay() const {
    if (!rack_ || !space_) return false;
    const engine::Engine* engine = app_.engine();
    if (engine && engine->running() && engine->transport().playing) return false;
    for (const auto& m : space_->scene().modules) {
        if (m.typeId == rack::host::kTransport) return true;
        if (!m.type) continue;
        for (const auto& tag : m.type->info().tags)
            if (tag == "sequencer" || tag == "clock") return true;
    }
    return false;
}

void RackWindow::showCapture() {
    if (stop_ && stop_->visible != capture_.has_value()) {
        stop_->visible = capture_.has_value();
        stop_->relayout();
    }
    if (status_) status_->setText(status());
}

void RackWindow::captureMenu(model::Id module, ui::Point at) {
    const model::Session& s = app_.head();
    std::vector<ui::MenuItem> items;
    for (const auto& e : s.tracks) {
        if (e.value->type != model::StripType::Channel) continue;
        items.push_back(
            {model::trackLabel(s, e.id), "",
             [this, module, at, track = e.id] {
                 const int bar = playheadBar(app_);
                 std::vector<ui::MenuItem> lengths;
                 lengths.push_back({"From bar " + std::to_string(bar), "", {}, false});
                 for (int bars : {1, 2, 4, 8, 16})
                     lengths.push_back(
                         {std::to_string(bars) + (bars == 1 ? " bar" : " bars"), "",
                          [this, module, track, bars] { captureNotes(module, track, bars); }});
                 root().openPopup(ui::makeMenu("rack.capture.length", std::move(lengths),
                                               [this] { root().closePopup(); }),
                                  {at.x, at.y, 1, 1});
             },
             true, false, false, root().theme().trackColor(e.value->color)});
    }
    if (items.empty()) items.push_back({"No channels to capture to", "", {}, false});
    root().openPopup(
        ui::makeMenu("rack.capture.track", std::move(items), [this] { root().closePopup(); }),
        {at.x, at.y, 1, 1});
}

void RackWindow::notesInputMenu(model::Id module, ui::Point at) {
    const model::RackModule* m = app_.head().modules.find(module);
    if (!m) return;
    auto set = [this, module](json::Value device) {
        json::Value args = json::Value::object();
        args["module"] = module.str();
        args["device"] = std::move(device);
        (void)exec("module.set", std::move(args));
    };
    const std::optional<std::string> current = m->device;
    std::vector<ui::MenuItem> items;
    items.push_back(
        {"None", "via its channel", [set] { set(json::Value()); }, true, false, !current});
    items.push_back({"Any MIDI input", "", [set] { set(json::Value("")); }, true, false,
                     current && current->empty()});
    // The devices the engine holds open (engine.inputs), as the channel
    // input menu lists them; a saved one it does not list stays shown.
    auto r = dispatcher_.execute({"engine.inputs", json::Value::object(), kOrigin});
    const json::Value* running = r ? r->find("running") : nullptr;
    const json::Value* midi = r ? r->find("midi") : nullptr;
    bool listed = !current || current->empty();
    if (running && running->isBool() && running->asBool() && midi && midi->isArray()) {
        if (!midi->asArray().empty()) items.push_back({"", "", {}, true, true});
        for (const auto& d : midi->asArray()) {
            if (!d.isString() || d.asString().empty()) continue;
            const std::string name = d.asString();
            listed = listed || name == *current;
            items.push_back({name, "", [set, name] { set(json::Value(name)); }, true, false,
                             current && *current == name});
        }
    } else {
        items.push_back({"", "", {}, true, true});
        items.push_back({"engine stopped \u2014 start it to list the devices", "", {}, false});
    }
    if (!listed) items.push_back({*current, "not connected", [] {}, true, false, true});
    root().openPopup(
        ui::makeMenu("rack.notes.input", std::move(items), [this] { root().closePopup(); }),
        {at.x, at.y, 1, 1});
}

void RackWindow::notesSourceMenu(model::Id module, ui::Point at) {
    const model::Session& s = app_.head();
    const model::RackModule* m = s.modules.find(module);
    const model::Rack* rack = m ? s.racks.find(m->rack) : nullptr;
    if (!m || !rack) return;
    auto set = [this, module](json::Value source) {
        json::Value args = json::Value::object();
        args["module"] = module.str();
        args["source"] = std::move(source);
        (void)exec("module.set", std::move(args));
    };
    std::vector<ui::MenuItem> items;
    items.push_back({"None", "", [set] { set(json::Value("none")); }, true, false, m->sourceOff});
    items.push_back({"Home", model::trackLabel(s, rack->track), [set] { set(json::Value()); }, true,
                     false, !m->sourceOff && !m->source,
                     root().theme().trackColor(s.tracks.find(rack->track)->color)});
    std::vector<ui::MenuItem> others;
    for (const auto& e : s.tracks) {
        if (e.value->type != model::StripType::Channel || e.id == rack->track) continue;
        others.push_back(
            {model::trackLabel(s, e.id), "", [set, id = e.id] { set(json::Value(id.str())); }, true,
             false, !m->sourceOff && m->source == e.id, root().theme().trackColor(e.value->color)});
    }
    if (!others.empty()) items.push_back({"", "", {}, true, true});
    for (auto& item : others) items.push_back(std::move(item));
    // Whether it also plays the MIDI sent into that track (ADR-0067).
    items.push_back({"", "", {}, true, true});
    items.push_back({"Hear MIDI sends", "",
                     [this, module, on = !m->sourceSends] {
                         json::Value args = json::Value::object();
                         args["module"] = module.str();
                         args["source_sends"] = on;
                         (void)exec("module.set", std::move(args));
                     },
                     !m->sourceOff, false, m->sourceSends});
    root().openPopup(
        ui::makeMenu("rack.notes.source", std::move(items), [this] { root().closePopup(); }),
        {at.x, at.y, 1, 1});
}

void RackWindow::moduleMenu(model::Id module, ui::Point at) {
    const model::RackModule* m = app_.head().modules.find(module);
    if (!m || !space_) return;
    const auto selected = space_->selection();
    const bool several = selected.size() > 1;
    std::vector<ui::MenuItem> items;
    auto setOn = [this](model::Id id, const char* field, json::Value v) {
        json::Value args = json::Value::object();
        args["module"] = id.str();
        args[field] = std::move(v);
        (void)exec("module.set", std::move(args));
    };
    if (m->fx && openEditor)
        items.push_back({"Open editor", "", [this, fx = *m->fx] {
                             if (auto r = openEditor(fx); !r) report(r.error());
                         }});
    items.push_back({"Rename…", "", [this, module] { rename(module); }, !several});
    items.push_back(
        {"Bypass", "",
         [this, module, setOn, bypass = m->bypass, selected] {
             auto r = dispatcher_.transact("Bypass", kOrigin, [&]() -> Result<json::Value> {
                 for (model::Id id : selected) {
                     json::Value args = json::Value::object();
                     args["module"] = id.str();
                     args["bypass"] = !bypass;
                     SUPPLE_TRY(dispatcher_.execute({"module.set", std::move(args), kOrigin}));
                 }
                 return json::Value::object();
             });
             if (!r) report(r.error());
             (void)module;
             (void)setOn;
         },
         true, false, m->bypass});
    items.push_back({"Copy", "Ctrl+C", [this] { copySelected(); }});
    items.push_back({"Cut", "Ctrl+X", [this] {
                         if (copySelected()) deleteSelected();
                     }});
    items.push_back({"Duplicate", "Ctrl+D", [this] { duplicateSelected(); }});
    const auto* type = catalog_ ? catalog_->find(m->type) : nullptr;
    const bool hasKnobs = type && !type->type->params().empty();
    items.push_back({"Randomize", "",
                     [this, module] {
                         json::Value args = json::Value::object();
                         args["module"] = module.str();
                         (void)exec("module.randomize", std::move(args));
                     },
                     hasKnobs && !several});
    // Presets: the type's factory patches.
    std::vector<std::string> presets;
    if (type && type->type->code())
        for (const auto& patch : type->type->code()->factoryPatches())
            presets.push_back(patch.name);
    if (!presets.empty())
        items.push_back({"Presets…", "",
                         [this, module, presets, at] {
                             std::vector<ui::MenuItem> list;
                             for (const auto& name : presets)
                                 list.push_back({name, "", [this, module, name] {
                                                     json::Value args = json::Value::object();
                                                     args["module"] = module.str();
                                                     args["preset"] = name;
                                                     (void)exec("module.set", std::move(args));
                                                 }});
                             root().openPopup(ui::makeMenu("rack.presets.menu", std::move(list),
                                                           [this] { root().closePopup(); }),
                                              {at.x, at.y, 1, 1});
                         },
                         !several});
    if (m->type == model::kNotesModule) {
        // What it plays (rack.md §2.2): its own MIDI input, and which track's MIDI.
        const std::string device = !m->device ? "none" : m->device->empty() ? "any" : *m->device;
        items.push_back({"MIDI input\u2026", device,
                         [this, module, at] { notesInputMenu(module, at); }, !several});
        const model::Session& s = app_.head();
        items.push_back({"Plays track\u2026", m->source ? model::trackLabel(s, *m->source) : "home",
                         [this, module, at] { notesSourceMenu(module, at); }, !several});
    }
    if (type && type->type->code())
        items.push_back({"Show source", "", [this, id = m->type] { showSource(id); }, !several});
    if (type && noteOutputs(*type->type))
        items.push_back({"Capture notes to track\u2026", "",
                         [this, module, at] { captureMenu(module, at); }, !several && !capture_});
    items.push_back({"Save as patch\u2026", "", [this, selected] {
                         savePreset(std::vector<model::Id>(selected.begin(), selected.end()));
                     }});
    items.push_back({"", "", {}, true, true});
    items.push_back({several ? "Delete modules" : "Delete", "Del", [this] { deleteSelected(); }});
    root().openPopup(
        ui::makeMenu("rack.module.menu", std::move(items), [this] { root().closePopup(); }),
        {at.x, at.y, 1, 1});
}

void RackWindow::spaceMenu(ui::Point at, ui::Point sp) {
    if (!catalog_) return;
    std::vector<ui::MenuItem> items;
    if (!rack_) {
        items.push_back({"Add rack", "", [this] { (void)addRack(); }});
    } else {
        // A search list at the pointer picks the module; it lands here.
        items.push_back({"Add module\u2026", "Type or double-click",
                         [this, at, sp] { quickAdd(at, sp, {}, std::nullopt, true); }});
        items.push_back({"Add plugin\u2026", "",
                         [this, sp] { (void)addModule(std::string(model::kPluginModule), sp); }});
        items.push_back({"Load patch\u2026", "", [this] { showPatches(true); }});
        const bool hasModules = space_ && !space_->scene().modules.empty();
        items.push_back({"Save rack as patch\u2026", "", [this] { savePreset({}); }, hasModules});
        items.push_back({"", "", {}, true, true});
        items.push_back({"Paste", "Ctrl+V",
                         [this, sp] {
                             if (auto r = paste(sp); !r) report(r.error());
                         },
                         app_.rackClipboard() != nullptr});
        items.push_back({"", "", {}, true, true});
        // With several modules selected, only they are tidied.
        const bool several = space_ && space_->selection().size() > 1;
        items.push_back({several ? "Tidy up selection" : "Tidy up", "", [this] { tidyUp(); },
                         several || (space_ && space_->scene().modules.size() > 1)});
        items.push_back({"Show everything", "Home", [this] {
                             if (space_) space_->frameAll();
                         }});
        const bool selected = space_ && !space_->selection().empty();
        items.push_back({"Show selection", "Shift+Home",
                         [this] {
                             if (space_) space_->frame(space_->selection());
                         },
                         selected});
    }
    root().openPopup(
        ui::makeMenu("rack.space.menu", std::move(items), [this] { root().closePopup(); }),
        {at.x, at.y, 1, 1});
}

void RackWindow::paramMenu(model::Id module, int index, ui::Point at) {
    const auto* m = space_ ? space_->scene().module(module) : nullptr;
    if (!m || !m->type || index < 0 || static_cast<std::size_t>(index) >= m->type->params().size())
        return;
    const auto& p = m->type->params()[static_cast<std::size_t>(index)];
    const double v = static_cast<std::size_t>(index) < m->values.size()
                         ? m->values[static_cast<std::size_t>(index)]
                         : p.def;
    racks::KnobMenuHooks hooks;
    hooks.exec = [this](const std::string& command, json::Value args) {
        (void)exec(command, std::move(args));
    };
    hooks.set = [this, module, index](double value) {
        param(module, index, value, ui::Gesture::Set);
    };
    hooks.learn = [this, module, index] { learn(module, index); };
    root().openPopup(ui::makeMenu("rack.param.menu",
                                  racks::knobMenu(app_.head(), module, p, v, std::move(hooks)),
                                  [this] { root().closePopup(); }),
                     {at.x, at.y, 1, 1});
}

void RackWindow::readPresets() {
    presetsRead_ = true;
    auto r = dispatcher_.execute({"rack.preset.list", json::Value::object(), kOrigin});
    presets_ = r ? std::move(*r) : json::Value::object();
}

bool RackWindow::rackEmpty() const {
    if (!rack_) return false;
    for (const auto& m : app_.head().modules)
        if (m.value->rack == *rack_ && m.value->type != model::kOutModule &&
            m.value->type != model::kInModule)
            return false;
    return true;
}

Status RackWindow::loadPreset(const std::string& name, std::optional<ui::Point> at) {
    if (!rack_) return {};
    json::Value args = json::Value::object();
    args["rack"] = rack_->str();
    args["name"] = name;
    if (at) {
        args["x"] = std::round(static_cast<double>(at->x));
        args["y"] = std::round(static_cast<double>(at->y));
    }
    if (rackEmpty()) {
        // A start: the patch's Outs and Ins are the rack's own (nothing is
        // replaced), moved to the patch's places by the load (its own
        // layout), and all of it shown.
        auto r = exec("rack.preset.load", std::move(args));
        if (!r) return r.error();
        sync();
        if (space_) {
            space_->select({});
            space_->frameAll();
        }
        return {};
    }
    auto r = exec("rack.preset.load", std::move(args));
    if (!r) return r.error();
    sync();
    // The preset's modules, selected and in view.
    std::set<model::Id> made;
    if (const json::Value* list = r->find("modules"); list && list->isArray())
        for (const auto& id : list->asArray())
            if (auto parsed = model::Id::parse(id.asString())) made.insert(*parsed);
    if (space_ && !made.empty()) {
        space_->select(made);
        space_->reveal(made);
    }
    return {};
}

// The rack (none: all of it) or its chosen modules, saved under a name asked
// for; a name in use is replaced only when the user says so.
void RackWindow::savePreset(std::optional<std::vector<model::Id>> modules) {
    if (!rack_ || !space_) return;
    std::string suggestion = model::rackLabel(app_.head(), *rack_);
    if (modules && modules->size() == 1)
        if (const auto* m = space_->scene().module(modules->front())) suggestion = m->title;
    const ui::Rect b = space_->bounds();
    const ui::Rect where{b.x + b.w / 2 - 120, b.y + 12, 240, 0};
    auto save = [this, modules](const std::string& name, bool replace) -> Result<json::Value> {
        json::Value args = json::Value::object();
        args["rack"] = rack_->str();
        args["name"] = name;
        if (modules) {
            json::Value ids = json::Value::array();
            for (const auto& id : *modules) ids.push_back(id.str());
            args["modules"] = std::move(ids);
        }
        if (replace) args["replace"] = true;
        return dispatcher_.execute({"rack.preset.save", std::move(args), kOrigin});
    };
    openNameField(
        root(), "rack.preset.name", suggestion, where,
        [this, save, where](const std::string& name) {
            auto r = save(name, false);
            if (!r && r.error().code == "rack.preset_exists") {
                std::vector<ui::MenuItem> confirm;
                confirm.push_back({"Replace \u201c" + name + "\u201d", "", [this, save, name] {
                                       if (auto again = save(name, true); !again)
                                           report(again.error());
                                       presetsSaved();
                                   }});
                confirm.push_back({"Cancel", "Esc", [] {}});
                root().openPopup(ui::makeMenu("rack.preset.replace", std::move(confirm),
                                              [this] { root().closePopup(); }),
                                 where);
                return;
            }
            if (!r) report(r.error());
            presetsSaved();
        });
}

void RackWindow::presetsSaved() {
    readPresets();
    version_ = ~std::uint64_t{0};
    sync();
}

void RackWindow::browserMenu(const std::string& type, ui::Point at) {
    std::vector<ui::MenuItem> items;
    if (type.starts_with(kPresetPrefix)) {
        const std::string name = type.substr(kPresetPrefix.size());
        items.push_back({"Load into rack", "Double-click",
                         [this, name] { (void)loadPreset(name, std::nullopt); },
                         rack_.has_value()});
        // Factory patches stay; deleting a saved one of the same name
        // brings the factory one back.
        bool factory = false;
        if (const json::Value* list = presets_.find("presets"); list && list->isArray())
            for (const auto& p : list->asArray())
                if (str(p, "name") == name)
                    if (const json::Value* f = p.find("factory"); f && f->isBool())
                        factory = f->asBool();
        if (!factory)
            items.push_back({"Delete patch", "", [this, name] {
                                 json::Value args = json::Value::object();
                                 args["name"] = name;
                                 (void)exec("rack.preset.delete", std::move(args));
                                 presetsSaved();
                             }});
    } else {
        items.push_back({"Add to rack", "Double-click", [this, type] { (void)addModule(type); }});
        const rack::Catalog::Entry* e = catalog_ ? catalog_->find(type) : nullptr;
        if (e && e->type->code())
            items.push_back({"Show source", "", [this, type] { showSource(type); }});
    }
    root().openPopup(
        ui::makeMenu("rack.browser.menu", std::move(items), [this] { root().closePopup(); }),
        {at.x, at.y, 1, 1});
}

void RackWindow::learn(model::Id module, int index) {
    const auto* m = space_ ? space_->scene().module(module) : nullptr;
    if (!m || !m->type || index < 0 || static_cast<std::size_t>(index) >= m->type->params().size())
        return;
    const auto& p = m->type->params()[static_cast<std::size_t>(index)];
    openLearnDialog(root(), actions_, app_,
                    racks::knobLearn(app_.head(), module, m->title, p,
                                     [this](const std::string& command, json::Value args) {
                                         (void)exec(command, std::move(args));
                                     }));
}

void RackWindow::tabMenu(model::Id rack, ui::Point at) {
    const model::Session& s = app_.head();
    std::vector<ui::MenuItem> items;
    items.push_back({"Rename…", "", [this, rack] { renameRack(rack); }});
    items.push_back({"", "", {}, true, true});
    const model::Rack* r = s.racks.find(rack);
    for (const auto& e : s.tracks) {
        if (e.value->type != model::StripType::Channel) continue;
        const bool home = r && r->track == e.id;
        items.push_back({"Home: " + model::trackLabel(s, e.id), "",
                         [this, rack, track = e.id] {
                             json::Value args = json::Value::object();
                             args["rack"] = rack.str();
                             args["track"] = track.str();
                             (void)exec("rack.set", std::move(args));
                         },
                         !home, false, home, root().theme().trackColor(e.value->color)});
    }
    items.push_back({"", "", {}, true, true});
    items.push_back({"Remove rack", "", [this, rack] {
                         json::Value args = json::Value::object();
                         args["rack"] = rack.str();
                         (void)exec("rack.remove", std::move(args));
                     }});
    root().openPopup(
        ui::makeMenu("rack.tab.menu", std::move(items), [this] { root().closePopup(); }),
        {at.x, at.y, 1, 1});
}

namespace {

// A name field over `where`: Enter submits, Escape and a click away cancel.
void openNameField(ui::Root& root, std::string id, std::string text, ui::Rect where,
                   std::function<void(const std::string&)> submit) {
    auto field = std::make_unique<ui::TextField>(std::move(id), std::move(text));
    ui::TextField* edit = field.get();
    edit->layout.width = ui::SizeRule::fixed(std::max(where.w, 160.0f));
    edit->layout.height = ui::SizeRule::fixed(24);
    edit->onSubmit = [&root, submit = std::move(submit)](const std::string& t) {
        const std::string copy = t;  // closing destroys the field
        root.closePopup();
        submit(copy);
    };
    edit->onCancel = [&root] { root.closePopup(); };
    root.openPopup(std::move(field), {where.x, where.y, where.w, 0});
    edit->selectAll();
    root.focus(edit);
}

}  // namespace

void RackWindow::rename(model::Id module) {
    const auto* m = space_ ? space_->scene().module(module) : nullptr;
    if (!m) return;
    const ui::Point a = space_->toWindow({static_cast<float>(m->x), static_cast<float>(m->y)});
    const float w = static_cast<float>(m->w * space_->view().zoom);
    const model::RackModule* mm = app_.head().modules.find(module);
    openNameField(root(), "rack.rename", mm && !mm->label.empty() ? mm->label : m->title,
                  {a.x, a.y, w, 0}, [this, module](const std::string& text) {
                      json::Value args = json::Value::object();
                      args["module"] = module.str();
                      args["label"] = text;
                      (void)exec("module.set", std::move(args));
                  });
}

void RackWindow::renameRack(model::Id rack) {
    if (!tabs_) return;
    const auto r = tabs_->tabRect(rack);
    const model::Rack* rk = app_.head().racks.find(rack);
    if (!r || !rk) return;
    openNameField(root(), "rack.tab.rename", model::rackLabel(app_.head(), rack),
                  {r->x, r->y, r->w, 0}, [this, rack](const std::string& text) {
                      json::Value args = json::Value::object();
                      args["rack"] = rack.str();
                      args["name"] = text;
                      (void)exec("rack.set", std::move(args));
                  });
}

void RackWindow::showSource(const std::string& type) {
    json::Value args = json::Value::object();
    args["type"] = type;
    auto got = exec("module.get", std::move(args));
    if (!got) return;
    const std::string origin = str(*got, "origin");
    const rack::Catalog::Entry* entry = catalog_ ? catalog_->find(type) : nullptr;
    const std::string title = entry ? entry->type->info().name : type;
    const bool library = origin == "library";
    // A library module's name: its file's.
    std::string name;
    if (library && entry) {
        const std::string path = entry->type->path();
        const auto slash = path.find_last_of("/\\");
        name = path.substr(slash == std::string::npos ? 0 : slash + 1);
        if (const auto dot = name.rfind('.'); dot != std::string::npos) name.resize(dot);
    }
    const ui::Rect win = root().content() ? root().content()->bounds() : ui::Rect{0, 0, 900, 600};
    auto form = std::make_unique<ui::Node>("rack.source");
    form->layout.dir = ui::Layout::Dir::Column;
    form->layout.width = ui::SizeRule::fixed(std::min(860.0f, win.w - 60));
    form->layout.height = ui::SizeRule::fixed(std::max(240.0f, win.h - 80));
    form->layout.padding = ui::Insets::all(12);
    form->layout.gap = 8;
    auto& head = form->emplace<ui::Node>("rack.source.head");
    head.layout.dir = ui::Layout::Dir::Row;
    head.layout.height = ui::SizeRule::fixed(26);
    head.layout.align = ui::Layout::Align::Center;
    head.layout.gap = 6;
    auto& label = head.emplace<ui::Label>(
        "rack.source.title", title + "  ·  " + origin + "  ·  " + type, ui::Font::UiBold, "body");
    label.layout.width = ui::SizeRule::fraction();
    ui::TextArea* code = nullptr;
    ui::Label* note = nullptr;
    if (library)
        head.emplace<ui::Button>("rack.source.save", "Save");
    else
        head.emplace<ui::Button>("rack.source.fork", "Make editable copy", [this, type, title] {
            std::string base;
            for (char ch : title)
                if (std::isalnum(static_cast<unsigned char>(ch)))
                    base += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (base.empty()) base = "module";
            // A free library name and id: "vco", then "vco_2"...
            std::string copy = base;
            for (int n = 2; catalog_ && catalog_->find("com.library." + copy); ++n)
                copy = base + "_" + std::to_string(n);
            json::Value a = json::Value::object();
            a["type"] = type;
            a["id"] = "com.library." + copy;
            a["name"] = copy;
            root().closePopup();
            auto r = exec("module.fork", std::move(a));
            if (!r) return;
            version_ = ~std::uint64_t{0};
            sync();
            showSource("com.library." + copy);
        });
    head.emplace<ui::Button>("rack.source.close", "Close", [this] { root().closePopup(); });
    code = &form->emplace<ui::TextArea>("rack.source.text", str(*got, "source"));
    code->layout.height = ui::SizeRule::fraction();
    note = &form->emplace<ui::Label>(
        "rack.source.status",
        library ? "Ctrl+Enter or Save compiles it; racks using it reload."
                : "A " + origin + " module: make an editable copy to change it.",
        ui::Font::Ui, "small", "ink-quiet");
    note->layout.height = ui::SizeRule::fixed(18);
    code->onCancel = [this] { root().closePopup(); };
    if (library) {
        auto compile = [this, name, code, note] {
            json::Value a = json::Value::object();
            a["name"] = name;
            a["source"] = code->text();
            auto r = dispatcher_.execute({"module.write", std::move(a), kOrigin});
            if (!r) {
                note->setText(r.error().message);
                return;
            }
            std::vector<ui::TextArea::Mark> marks;
            if (const json::Value* d = r->find("diagnostics"); d && d->isArray())
                for (const auto& x : d->asArray()) {
                    ui::TextArea::Mark mk;
                    if (const auto* l = x.find("line"); l && l->isNumber())
                        mk.line = static_cast<int>(l->asInt());
                    if (const auto* c = x.find("column"); c && c->isNumber())
                        mk.column = static_cast<int>(c->asInt());
                    mk.message = str(x, "message");
                    if (const std::string hint = str(x, "hint"); !hint.empty())
                        mk.message += " (" + hint + ")";
                    mk.error = str(x, "severity") != "warning";
                    marks.push_back(std::move(mk));
                }
            const json::Value* ok = r->find("ok");
            const json::Value* reloaded = r->find("reloaded");
            if (ok && ok->isBool() && ok->asBool())
                note->setText(
                    "Compiled; " +
                    std::to_string(reloaded && reloaded->isNumber() ? reloaded->asInt() : 0) +
                    " playing modules reloaded.");
            else
                note->setText(marks.empty() ? std::string("It does not compile.")
                                            : "Line " + std::to_string(marks.front().line) + ": " +
                                                  marks.front().message);
            // The caret to the first mistake, which scrolls it into view.
            if (!marks.empty() && marks.front().line > 0) {
                const std::string& t = code->text();
                std::size_t at = 0;
                for (int line = 1; line < marks.front().line && at != std::string::npos; ++line)
                    if ((at = t.find('\n', at)) != std::string::npos) ++at;
                if (at != std::string::npos) code->setCaret(at);
            }
            code->setMarks(std::move(marks));
            version_ = ~std::uint64_t{0};
            sync();
        };
        code->onSubmit = [compile](const std::string&) { compile(); };
        if (auto* b = static_cast<ui::Button*>(head.find("rack.source.save"))) b->onClick = compile;
    }
    root().openPopup(std::move(form), {}, ui::Placement::Center);
    root().focus(code);
}

}  // namespace supple::gui
