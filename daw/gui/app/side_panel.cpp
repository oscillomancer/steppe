// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "side_panel.h"

#include <algorithm>
#include <utility>

#include "supple/ui/canvas.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

// The panel's left edge: drag to set the width (left is wider), double-click
// for the default. It lights up under the pointer and while dragged, as the
// docks' grips do.
class SideGrip : public ui::Node {
  public:
    explicit SideGrip(std::string id) : Node(std::move(id)) {
        interactive = true;
        layout.width = ui::SizeRule::fixed(5);
    }
    std::function<float()> width;
    std::function<void(float)> resize;
    std::function<void()> reset;
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        const bool live = hovered() || dragging_;
        const ui::Rect r = bounds();
        c.fillRect(r, live ? t.color("selection") : t.color("panel"));
        c.fillRect({r.x, r.y, 1, r.h}, t.color("rule"));
        if (live) c.fillRect({r.x + 1, r.y + r.h / 2 - 16, 2, 32}, t.color("focus"));
    }
    void onHover(bool) override { invalidate(); }
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.type == ui::PointerEvent::Type::Down && e.button == 1) {
            if (e.clicks >= 2) {
                if (reset) reset();
                return true;
            }
            dragging_ = true;
            pressX_ = e.pos.x;
            start_ = width ? width() : 0;
            invalidate();
            return true;
        }
        if (e.type == ui::PointerEvent::Type::Move && dragging_) {
            if (resize) resize(start_ + (pressX_ - e.pos.x));
            return true;
        }
        if ((e.type == ui::PointerEvent::Type::Up || e.type == ui::PointerEvent::Type::Cancel) &&
            dragging_) {
            dragging_ = false;
            invalidate();
            return true;
        }
        return false;
    }

  private:
    bool dragging_ = false;
    float pressX_ = 0, start_ = 0;
};

// A tab's title in the bar: ink with an accent line under it while its tab is
// in view, quiet ink otherwise.
class TabButton : public ui::Node {
  public:
    TabButton(std::string id, std::string title, std::function<void()> pick)
        : Node(std::move(id), ui::Role::Button), title_(std::move(title)), pick_(std::move(pick)) {
        interactive = true;
        layout.width = ui::SizeRule::content();
    }
    bool active = false;
    ui::Size measure(const ui::Canvas& c, ui::Size) override {
        return {c.textWidth(title_, ui::Font::UiBold, c.theme().type("small")) + 16, 30};
    }
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        const ui::Rect r = bounds();
        c.text(r, title_, ui::Font::UiBold, t.type("small"),
               t.color(active || hovered() ? "ink" : "ink-quiet"), ui::kCenter | ui::kMiddle);
        if (active) c.fillRect({r.x + 8, r.bottom() - 2, r.w - 16, 2}, t.color("accent"));
    }
    void onHover(bool) override { invalidate(); }
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.type == ui::PointerEvent::Type::Down && e.button == 1) {
            if (pick_) pick_();
            return true;
        }
        return e.type == ui::PointerEvent::Type::Up;
    }
    std::string valueText() const override { return title_ + (active ? " (shown)" : ""); }

  private:
    std::string title_;
    std::function<void()> pick_;
};

// The panel's ground, and the rule under the tab bar.
class Ground : public ui::Node {
  public:
    using Node::Node;
    bool rule = false;
    void paint(ui::Canvas& c) override {
        const ui::Rect r = bounds();
        c.fillRect(r, c.theme().color("panel"));
        if (rule) c.fillRect({r.x, r.bottom() - 1, r.w, 1}, c.theme().color("rule"));
    }
};

}  // namespace

SidePanel::SidePanel(ui::Root& root) : root_(root) {}

void SidePanel::addTab(std::string id, std::string title, std::unique_ptr<ui::Node> content,
                       std::function<void(bool)> shown) {
    Tab tab;
    tab.id = std::move(id);
    tab.title = std::move(title);
    tab.content = content.get();
    tab.pending = std::move(content);
    tab.shown = std::move(shown);
    if (tab_.empty()) tab_ = tab.id;
    tabs_.push_back(std::move(tab));
}

std::unique_ptr<ui::Node> SidePanel::build() {
    auto panel = std::make_unique<ui::Node>("side");
    panel_ = panel.get();
    panel->layout.dir = ui::Layout::Dir::Row;
    panel->layout.width = ui::SizeRule::fixed(width_);
    panel->visible = false;
    auto& grip = panel->emplace<SideGrip>("side.grip");
    grip.width = [this] { return width_; };
    grip.resize = [this](float w) { setWidth(w); };
    grip.reset = [this] { setWidth(kDefaultWidth); };
    auto& body = panel->emplace<Ground>("side.body");
    body.layout.dir = ui::Layout::Dir::Column;
    body.layout.width = ui::SizeRule::fraction();
    auto& bar = body.emplace<Ground>("side.tabs");
    bar.rule = true;
    bar.layout.dir = ui::Layout::Dir::Row;
    bar.layout.height = ui::SizeRule::fixed(31);
    bar.layout.padding = ui::Insets::xy(4, 0);
    bar.layout.gap = 2;
    bar.layout.align = ui::Layout::Align::Center;
    for (Tab& tab : tabs_)
        tab.button = &bar.emplace<TabButton>("side.tab." + tab.id, tab.title,
                                             [this, id = tab.id] { show(id); });
    bar.emplace<ui::Node>("side.tabs.rest").layout.width = ui::SizeRule::fraction();
    auto& close = bar.emplace<ui::Button>("side.close", "Close", [this] { hide(); });
    close.quiet = true;
    close.layout.height = ui::SizeRule::fixed(24);
    for (Tab& tab : tabs_) {
        tab.content->visible = false;
        tab.content->layout.width = ui::SizeRule::fraction();
        tab.content->layout.height = ui::SizeRule::fraction();
        body.add(std::move(tab.pending));
    }
    return panel;
}

bool SidePanel::visible() const { return panel_ && panel_->visible; }

void SidePanel::apply(bool visible, const std::string& tab) {
    if (!panel_) return;
    const auto known = std::find_if(tabs_.begin(), tabs_.end(),
                                    [&](const Tab& t) { return t.id == tab; }) != tabs_.end();
    if (known) tab_ = tab;
    if (!visible) root_.releaseWithin(panel_);
    panel_->visible = visible;
    for (Tab& t : tabs_) {
        const bool on = visible && t.id == tab_;
        // A tab out of view keeps no focus or drag: keys must not reach it.
        if (!on && t.content->visible) root_.releaseWithin(t.content);
        if (t.shown && t.content->visible != on) t.shown(on);
        t.content->visible = on;
        static_cast<TabButton*>(t.button)->active = t.id == tab_;
    }
    root_.relayout();
    root_.damageAll();
}

void SidePanel::show(const std::string& tab) { apply(true, tab); }
void SidePanel::hide() { apply(false, tab_); }
void SidePanel::toggle(const std::string& tab) {
    if (showing(tab))
        hide();
    else
        show(tab);
}

void SidePanel::setWidth(float width) {
    width = std::clamp(width, kMinWidth, kMaxWidth);
    if (width == width_) return;
    width_ = width;
    if (!panel_) return;
    panel_->layout.width = ui::SizeRule::fixed(width_);
    root_.relayout();
    root_.damageAll();
}

json::Value SidePanel::save() const {
    json::Value v = json::Value::object();
    v["visible"] = visible();
    v["tab"] = tab_;
    v["width"] = static_cast<double>(width_);
    return v;
}

void SidePanel::load(const json::Value& state) {
    if (!state.isObject()) return;
    if (const json::Value* w = state.find("width"); w && w->isNumber())
        setWidth(static_cast<float>(w->asDouble()));
    std::string tab = tab_;
    if (const json::Value* t = state.find("tab"); t && t->isString()) tab = t->asString();
    bool open = visible();
    if (const json::Value* v = state.find("visible"); v && v->isBool()) open = v->asBool();
    apply(open, tab);
}

}  // namespace supple::gui
