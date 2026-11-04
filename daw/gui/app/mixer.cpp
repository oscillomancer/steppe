// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "mixer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <utility>

#include <SDL3/SDL_keycode.h>

#include "panel_grip.h"
#include "panel_height.h"
#include "strip_learn.h"
#include "supple/model/groups.h"
#include "supple/ui/controls.h"
#include "supple/ui/list.h"
#include "supple/ui/popup.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"
#include "supple/views/track_layout.h"

namespace supple::gui {
namespace {

const cmd::Origin kOrigin{cmd::Origin::Type::Ui, "gui", {}};
constexpr float kStripW = 92;
// The strip's chain section is the only one that follows the panel's height;
// the rest (name, sends, arm, in/out, polarity/monitor/mono, pan, fader,
// value, M/S, gaps, padding) is fixed.
constexpr float kSlotH = 18, kSlotGap = 2;
constexpr float kGripH = 5, kHeaderH = 30;
constexpr float kFaderH = 133;  // the fader and meter row
// The fixed rows of a channel with no sends, their gaps and the padding (each
// send adds kSendH; busses have no arm row).
constexpr float kStripChrome = 173;
constexpr float kSendH = 14;
constexpr float kDragThreshold = 4;

// fx.move's index (among the chain without the plug-in) for dropping the
// plug-in at `from` into the gap before slot `gap`; nullopt when it stays.
std::optional<std::size_t> chainMoveIndex(std::size_t from, std::size_t gap) {
    const std::size_t index = gap > from ? gap - 1 : gap;
    if (index == from) return std::nullopt;
    return index;
}

constexpr float chainRows(int n) { return n <= 0 ? 0 : n * kSlotH + (n - 1) * kSlotGap; }
constexpr float panelHeight(int slots) {
    return kGripH + kHeaderH + kStripChrome + kFaderH + chainRows(slots);
}
constexpr int kDefaultSlots = 5, kMinSlots = 2;

std::string dbText(double db) {
    if (db <= ui::kFaderSilenceDb + 1e-9) return "-inf";
    char buf[24];
    std::snprintf(buf, sizeof buf, "%+.1f", db);
    return buf;
}

std::string sendText(const model::Session& session, const model::Route& route) {
    std::string to = "?";
    if (route.to.track && session.tracks.find(*route.to.track))
        to = model::trackLabel(session, *route.to.track);
    if (route.to.fx) to = route.to.fx->str() + " key";
    return "→ " + to + " " + dbText(route.volumeDb);
}

// A left-button press that becomes a drag once it moves a few pixels; a press
// that never does stays a click for the node (nothing is reported then).
class DragTracker {
  public:
    using Phase = Mixer::DragPhase;
    std::function<void(Phase, ui::Point)> notify;
    unsigned mods = 0;  // the latest pointer event's modifiers
    bool down() const { return down_; }
    bool dragging() const { return dragging_; }
    void press(ui::Point p) {
        down_ = true;
        dragging_ = false;
        origin_ = p;
    }
    // A move: true while a drag is under way.
    bool move(ui::Point p) {
        if (!down_) return false;
        if (!dragging_) {
            if (std::hypot(p.x - origin_.x, p.y - origin_.y) < kDragThreshold) return false;
            dragging_ = true;
            if (notify) notify(Phase::Begin, p);
        }
        if (notify) notify(Phase::Move, p);
        return true;
    }
    // A release: true when it ended a drag (so it is not a click).
    bool release(ui::Point p) {
        const bool was = dragging_;
        down_ = dragging_ = false;
        if (was && notify) notify(Phase::End, p);
        return was;
    }
    bool cancel() {
        const bool was = dragging_;
        down_ = dragging_ = false;
        if (was && notify) notify(Phase::Cancel, {});
        return was;
    }

  private:
    bool down_ = false, dragging_ = false;
    ui::Point origin_;
};

// A button that can also be dragged: a press that moves becomes a drag (no
// click then); one that does not stays a click.
class DragButton : public ui::Button {
  public:
    using Button::Button;
    DragTracker drag;
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.type == ui::PointerEvent::Type::Cancel) {
            drag.cancel();
            return Button::onPointer(e);
        }
        const bool left = e.button == 1;
        drag.mods = e.mods;
        if (e.type == ui::PointerEvent::Type::Down && left) drag.press(e.pos);
        if (e.type == ui::PointerEvent::Type::Move && drag.move(e.pos)) return true;
        if (e.type == ui::PointerEvent::Type::Up && left && drag.release(e.pos)) {
            // Release the button without a click: the pointer is "outside" it.
            Button::onPointer({e.type, {-1e6f, -1e6f}, e.button, e.clicks, e.mods});
            return true;
        }
        return Button::onPointer(e);
    }
    bool onKey(const ui::KeyEvent& e) override {
        if (e.key == SDLK_ESCAPE && drag.cancel()) {
            Button::onPointer({ui::PointerEvent::Type::Up, {-1e6f, -1e6f}, 1, 1, 0});
            if (root()) root()->capture(nullptr);
            return true;
        }
        return Button::onKey(e);
    }
};

// A plug-in in a strip's chain: a click opens its editor (a button), a drag
// reorders it within the chain or copies it onto another strip (with Ctrl,
// the whole chain), a right-click opens its own menu.
class FxSlot : public DragButton {
  public:
    FxSlot(std::string id, std::string text, std::function<void()> click, std::size_t at)
        : DragButton(std::move(id), std::move(text), std::move(click)), chainIndex(at) {}
    const std::size_t chainIndex;         // its place in the track's chain
    std::function<void()> remove;         // Alt+click
    std::function<void(ui::Point)> menu;  // a right-click (not the track's menu)
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.type == ui::PointerEvent::Type::Down && e.button == 1 && (e.mods & ui::kAlt) &&
            remove) {
            remove();  // the press goes no further: no click, no drag
            return true;
        }
        if (e.type == ui::PointerEvent::Type::Down && e.button == 3 && menu) {
            menu(e.pos);
            return true;
        }
        return DragButton::onPointer(e);
    }
};

// A strip's name: a double-click edits it in place; Enter or a click anywhere
// else keeps the edit, Escape drops it. A single press goes on to the strip.
class NameEdit : public ui::Node {
  public:
    NameEdit(const std::string& tid, const std::string& name,
             std::function<void(const std::string&)> commit)
        : Node("mixer.nameedit." + tid), commit_(std::move(commit)) {
        interactive = true;
        layout.width = ui::SizeRule::fraction();
        label_ = &emplace<ui::Label>("mixer.name." + tid, name, ui::Font::UiBold, "caption");
        label_->align = ui::kLeft | ui::kMiddle;
        field_ = &emplace<ui::TextField>("mixer.rename." + tid, name);
        field_->visible = false;
        field_->onSubmit = [this](const std::string& text) { finish(text, true); };
        field_->onBlur = [this](const std::string& text) { finish(text, true); };
        field_->onCancel = [this] { finish({}, false); };
    }
    ui::Label* label() const { return label_; }
    bool editing() const { return editing_; }
    void start() {
        editing_ = true;
        field_->setText(label_->text());
        field_->selectAll();
        field_->visible = true;
        label_->visible = false;
        if (ui::Root* r = root()) {
            r->relayout();
            r->focus(field_);
        }
        invalidate();
    }
    void arrange(const ui::Canvas& canvas, const ui::Rect& r) override {
        setBounds(r);
        label_->arrange(canvas, r);
        field_->arrange(canvas, {r.x, r.y - 1, r.w, r.h + 2});
    }
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.type != ui::PointerEvent::Type::Down || e.button != 1 || e.clicks < 2 || editing_)
            return false;
        start();
        return true;
    }

  private:
    void finish(const std::string& text, bool keep) {
        if (!editing_) return;
        editing_ = false;
        field_->visible = false;
        label_->visible = true;
        if (ui::Root* r = root()) {
            r->releaseWithin(field_);  // Space plays again
            r->relayout();
        }
        invalidate();
        if (keep && text != label_->text() && commit_) commit_(text);
    }
    ui::Label* label_ = nullptr;
    ui::TextField* field_ = nullptr;
    std::function<void(const std::string&)> commit_;
    bool editing_ = false;
};

// A strip's plug-ins, one per row, scrolled by the wheel or a bar when the
// chain is longer than the room the panel gives it.
class FxList : public ui::Node {
  public:
    explicit FxList(std::string id) : Node(std::move(id)) {
        clipsChildren = true;
        interactive = true;  // its empty part takes a double-click
        layout.height = ui::SizeRule::fraction();
        auto bar =
            std::make_unique<ui::ScrollBar>(this->id() + ".bar", ui::ScrollBar::Axis::Vertical);
        bar_ = bar.get();
        bar_->onScroll = [this](float v) {
            scroll_ = v;
            relayout();
            invalidate();
        };
        add(std::move(bar));
    }
    FxSlot& addSlot(std::unique_ptr<FxSlot> slot) {
        FxSlot& ref = *slot;
        slots_.push_back(&ref);
        add(std::move(slot));
        return ref;
    }
    const std::vector<FxSlot*>& slots() const { return slots_; }
    float content() const { return chainRows(static_cast<int>(slots_.size())); }
    void arrange(const ui::Canvas& canvas, const ui::Rect& b) override {
        setBounds(b);
        const bool bar = content() > b.h + 0.5f;
        scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, content() - b.h));
        bar_->visible = bar;
        const float w = b.w - (bar ? ui::ScrollBar::kThickness + 2 : 0);
        bar_->setRange(content(), b.h);
        bar_->setValue(scroll_);
        bar_->arrange(canvas,
                      {b.right() - ui::ScrollBar::kThickness, b.y, ui::ScrollBar::kThickness, b.h});
        float y = b.y - scroll_;
        for (FxSlot* slot : slots_) {
            slot->arrange(canvas, {b.x, y, w, kSlotH});
            y += kSlotH + kSlotGap;
        }
    }
    std::function<void()> onDoubleClick;  // on the list's empty part: add a plug-in
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.type == ui::PointerEvent::Type::Down && e.button == 1 && e.clicks >= 2 &&
            onDoubleClick && std::none_of(slots_.begin(), slots_.end(), [&](const FxSlot* slot) {
                return slot->bounds().contains(e.pos);
            })) {
            onDoubleClick();
            return true;
        }
        if (e.type != ui::PointerEvent::Type::Wheel || content() <= bounds().h + 0.5f) return false;
        scroll_ = std::max(0.0f, scroll_ - e.wheelY * (kSlotH + kSlotGap));
        relayout();
        invalidate();
        return true;
    }

  private:
    std::vector<FxSlot*> slots_;
    ui::ScrollBar* bar_ = nullptr;
    float scroll_ = 0;
};

// A send under a strip's plug-ins: a click opens the routing matrix, Alt+click
// deletes it, a right-click opens its menu (Delete). Its text brightens under
// the pointer, to show it can be clicked.
class SendRow : public ui::Button {
  public:
    SendRow(std::string id, std::string text, bool muted, std::function<void()> click)
        : Button(std::move(id), std::move(text), std::move(click)), muted_(muted) {}
    std::function<void()> remove;         // Alt+click
    std::function<void(ui::Point)> menu;  // a right-click (not the track's menu)
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.type == ui::PointerEvent::Type::Down && e.button == 1 && (e.mods & ui::kAlt) &&
            remove) {
            remove();
            return true;
        }
        if (e.type == ui::PointerEvent::Type::Down && e.button == 3 && menu) {
            menu(e.pos);
            return true;
        }
        return Button::onPointer(e);
    }
    // The text's colour: quiet (faint when muted), a third of the way to the
    // full ink while hovered or pressed.
    ui::Color ink(const ui::Theme& t) const {
        const ui::Color base = t.color(muted_ ? "ink-faint" : "ink-quiet");
        if (!hovered() && !pressed()) return base;
        const ui::Color full = t.color(muted_ ? "ink-quiet" : "ink");
        const auto mix = [](float a, float b) { return a + (b - a) * 0.4f; };
        return {mix(base.r, full.r), mix(base.g, full.g), mix(base.b, full.b), base.a};
    }
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        c.text(bounds(), valueText(), ui::Font::Ui, t.type("caption"), ink(t),
               ui::kLeft | ui::kMiddle);
    }

  private:
    bool muted_ = false;
};

// A strip's sends, one per row, up to two in view; more scroll by the wheel or
// a bar.
class SendList : public ui::Node {
  public:
    static constexpr int kShown = 2;
    explicit SendList(std::string id) : Node(std::move(id)) {
        clipsChildren = true;
        interactive = true;  // the wheel scrolls it
        auto bar =
            std::make_unique<ui::ScrollBar>(this->id() + ".bar", ui::ScrollBar::Axis::Vertical);
        bar_ = bar.get();
        bar_->onScroll = [this](float v) {
            scroll_ = v;
            relayout();
            invalidate();
        };
        add(std::move(bar));
    }
    SendRow& addRow(std::unique_ptr<SendRow> row) {
        SendRow& ref = *row;
        rows_.push_back(&ref);
        add(std::move(row));
        const auto n = static_cast<float>(std::min<std::size_t>(rows_.size(), kShown));
        layout.height = ui::SizeRule::fixed(n * kSendH);
        return ref;
    }
    const std::vector<SendRow*>& rows() const { return rows_; }
    float content() const { return static_cast<float>(rows_.size()) * kSendH; }
    float scroll() const { return scroll_; }
    void arrange(const ui::Canvas& canvas, const ui::Rect& b) override {
        setBounds(b);
        const bool bar = content() > b.h + 0.5f;
        scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, content() - b.h));
        bar_->visible = bar;
        const float w = b.w - (bar ? ui::ScrollBar::kThickness + 2 : 0);
        bar_->setRange(content(), b.h);
        bar_->setValue(scroll_);
        bar_->arrange(canvas,
                      {b.right() - ui::ScrollBar::kThickness, b.y, ui::ScrollBar::kThickness, b.h});
        float y = b.y - scroll_;
        for (SendRow* row : rows_) {
            row->arrange(canvas, {b.x, y, w, kSendH});
            y += kSendH;
        }
    }
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.type != ui::PointerEvent::Type::Wheel || content() <= bounds().h + 0.5f) return false;
        scroll_ = std::clamp(scroll_ - e.wheelY * kSendH, 0.0f, content() - bounds().h);
        relayout();
        invalidate();
        return true;
    }

  private:
    std::vector<SendRow*> rows_;
    ui::ScrollBar* bar_ = nullptr;
    float scroll_ = 0;
};

// A run of strips side by side from `scroll` on (the scrolled ones clip).
class StripGroup : public ui::Node {
  public:
    explicit StripGroup(std::string id, bool clip) : Node(std::move(id)) { clipsChildren = clip; }
    float scroll = 0;
    float content() const {
        float w = 0;
        for (const auto& c : children())
            if (c->visible) w += kStripW + 4;
        return w;
    }
    void arrange(const ui::Canvas& canvas, const ui::Rect& b) override {
        setBounds(b);
        float x = b.x - scroll;
        for (const auto& c : children()) {
            if (!c->visible) continue;
            c->arrange(canvas, {x, b.y, kStripW, b.h});
            x += kStripW + 4;
        }
    }
};

// Strips side by side, scrolled horizontally (wheel or the bar); the masters
// stay pinned to the right edge.
class StripRow : public ui::Node {
  public:
    StripRow() : Node("mixer.strips") {
        interactive = true;
        layout.height = ui::SizeRule::fraction();
        scrolled_ = &emplace<StripGroup>("mixer.scrolled", true);
        pinned_ = &emplace<StripGroup>("mixer.pinned", false);
        auto bar = std::make_unique<ui::ScrollBar>("mixer.scroll", ui::ScrollBar::Axis::Horizontal);
        bar_ = bar.get();
        bar_->onScroll = [this](float v) {
            scrolled_->scroll = v;
            relayout();
            invalidate();
        };
        add(std::move(bar));
    }
    void clear() {
        for (StripGroup* g : {scrolled_, pinned_}) {
            std::vector<ui::Node*> old;
            for (const auto& c : g->children()) old.push_back(c.get());
            for (ui::Node* n : old) (void)g->remove(n);
        }
    }
    void addStrip(std::unique_ptr<ui::Node> strip, bool pinned) {
        (pinned ? pinned_ : scrolled_)->add(std::move(strip));
    }
    void arrange(const ui::Canvas& canvas, const ui::Rect& b) override {
        setBounds(b);
        const float pinnedW = std::min(b.w, pinned_->content());
        const float areaW = b.w - pinnedW;
        const float content = scrolled_->content();
        const float barH = content > areaW ? ui::ScrollBar::kThickness : 0;
        scrolled_->scroll = std::clamp(scrolled_->scroll, 0.0f, std::max(0.0f, content - areaW));
        bar_->setRange(content, areaW);
        bar_->setValue(scrolled_->scroll);
        bar_->visible = barH > 0;
        bar_->arrange(canvas, {b.x, b.bottom() - barH, areaW, barH});
        scrolled_->arrange(canvas, {b.x, b.y, areaW, b.h - barH});
        pinned_->arrange(canvas, {b.right() - pinnedW, b.y, pinnedW, b.h});
    }
    void paint(ui::Canvas& c) override { c.fillRect(bounds(), c.theme().color("ground")); }
    std::function<void()> onDoubleClick;           // on the row's empty space
    std::function<void(ui::Point)> onContextMenu;  // a right-click there
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.type == ui::PointerEvent::Type::Down && e.button == 1 && e.clicks >= 2 &&
            onDoubleClick && !overStrip(e.pos)) {
            onDoubleClick();
            return true;
        }
        if (e.type == ui::PointerEvent::Type::Down && e.button == 3 && onContextMenu &&
            !overStrip(e.pos)) {
            onContextMenu(e.pos);
            return true;
        }
        if (e.type != ui::PointerEvent::Type::Wheel) return false;
        scrolled_->scroll = std::max(0.0f, scrolled_->scroll - (e.wheelY + e.wheelX) * 60);
        relayout();
        invalidate();
        return true;
    }

  private:
    void relayout() {
        if (ui::Root* r = root()) r->relayout();
    }
    // A strip (as far as its group shows it) or the scroll bar is under `p`.
    bool overStrip(ui::Point p) const {
        if (bar_->visible && bar_->bounds().contains(p)) return true;
        for (const StripGroup* g : {scrolled_, pinned_})
            for (const auto& c : g->children())
                if (c->visible && c->bounds().intersect(g->bounds()).contains(p)) return true;
        return false;
    }
    StripGroup* scrolled_ = nullptr;
    StripGroup* pinned_ = nullptr;
    ui::ScrollBar* bar_ = nullptr;
};

// The track's colour, as a stripe under the strip's name (nothing when none).
class ColorStripe : public ui::Node {
  public:
    ColorStripe(std::string id, std::string colour)
        : Node(std::move(id)), colour_(std::move(colour)) {
        layout.height = ui::SizeRule::fixed(3);
    }
    void paint(ui::Canvas& c) override {
        if (const auto colour = c.theme().trackColor(colour_)) c.fillRect(bounds(), *colour);
    }

  private:
    std::string colour_;
};

// A frozen channel's mark: a snowflake under the strip's name.
class FrozenMark : public ui::Node {
  public:
    explicit FrozenMark(std::string id) : Node(std::move(id)) {
        layout.height = ui::SizeRule::fixed(14);
    }
    void paint(ui::Canvas& c) override {
        const ui::Rect b = bounds();
        ui::drawSnowflake(c, {b.x + 9, b.y + b.h / 2}, 6.0f, c.theme().color("ink"));
    }
};

// A strip's background: the type's colour along the top; a right-click anywhere
// on it opens the track menu, and a drag from a part no control uses moves the
// strip among its own kind.
class StripFrame : public ui::Node {
  public:
    StripFrame(std::string id, std::string colorToken, std::function<void(ui::Point)> menu)
        : Node(std::move(id)), color_(std::move(colorToken)), menu_(std::move(menu)) {
        interactive = true;
        layout.dir = ui::Layout::Dir::Column;
        layout.padding = ui::Insets::all(5);
        layout.gap = 4;
    }
    DragTracker drag;  // notify unset: this strip cannot be dragged
    // The track selection the TCP shares (gui.md §4.2): a press on the strip's
    // empty part selects (Ctrl toggles, Shift extends); a right-click keeps a
    // selection the strip is part of.
    std::function<void(unsigned mods)> select;
    std::function<bool()> selected;
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        c.fillRect(bounds(), t.color("panel"));
        if (selected && selected()) {  // highlighted and outlined, as in the TCP
            c.fillRect(bounds(), t.color("selection").withAlpha(0.55f));
            c.strokeRect({bounds().x + 0.5f, bounds().y + 0.5f, bounds().w - 1, bounds().h - 1},
                         t.color("focus"), 1.0f);
        }
        c.fillRect({bounds().x, bounds().y, bounds().w, 4}, t.color(color_));
        c.fillRect({bounds().right() - 1, bounds().y, 1, bounds().h}, t.color("rule"));
    }
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.type == ui::PointerEvent::Type::Down && e.button == 3) {
            if (select && !(selected && selected())) select(0);
            if (menu_) menu_(e.pos);
            return true;
        }
        // A plain press on a selected strip keeps the selection (a drag moves
        // all of it); a release without a drag selects it alone.
        if (e.type == ui::PointerEvent::Type::Down && e.button == 1 && select) {
            deferSelect_ =
                selected && selected() && drag.notify && !(e.mods & (ui::kShift | ui::kCtrl));
            select(deferSelect_ ? views::kSelectKeep : e.mods);
        }
        if (e.type == ui::PointerEvent::Type::Cancel) {
            drag.cancel();
            return true;
        }
        if (!drag.notify) return e.type == ui::PointerEvent::Type::Down && e.button == 1 && select;
        if (e.type == ui::PointerEvent::Type::Down && e.button == 1) {
            drag.press(e.pos);
            return true;
        }
        if (e.type == ui::PointerEvent::Type::Move && drag.down()) {
            const bool was = drag.dragging();
            const bool now = drag.move(e.pos);
            if (now && !was && root()) root()->focus(this);  // Escape cancels
            return now;
        }
        if (e.type == ui::PointerEvent::Type::Up && e.button == 1 && drag.down()) {
            const bool dragged = drag.release(e.pos);
            if (root() && root()->focused() == this) root()->focus(nullptr);
            if (std::exchange(deferSelect_, false) && !dragged && select) select(0);
            return true;
        }
        return false;
    }
    bool onKey(const ui::KeyEvent& e) override {
        if (e.key == SDLK_ESCAPE && drag.cancel()) {
            if (root()) {
                root()->capture(nullptr);
                root()->focus(nullptr);
            }
            return true;
        }
        return false;
    }

  private:
    std::string color_;
    std::function<void(ui::Point)> menu_;
    bool deferSelect_ = false;
};

// The drag's feedback, over everything: the dragged strip dimmed, an insertion
// bar where it would land, or the strip a plug-in would be copied onto; a plug-in
// drag carries a chip with its name.
class DragGhost : public ui::Node {
  public:
    explicit DragGhost(std::shared_ptr<const Mixer::DragView> view)
        : Node("mixer.drag"), view_(std::move(view)) {}
    void arrange(const ui::Canvas&, const ui::Rect&) override {
        if (parent()) setBounds(parent()->bounds());
    }
    void paint(ui::Canvas& c) override {
        const Mixer::DragView& d = *view_;
        if (!d.active) return;
        const ui::Theme& t = c.theme();
        const ui::Color accent = t.color("focus");
        if (d.kind == Mixer::DragView::Kind::Send) {
            // As the TCP's: the strip it would feed outlined, a wire from "out".
            if (d.drop == Mixer::DragView::Drop::Copy) {
                c.fillRect(d.marker, accent.withAlpha(0.2f));
                c.strokeRect(d.marker.inset(0.5f), accent, 1);
            }
            c.line({d.source.x + d.source.w / 2, d.source.y + d.source.h / 2}, d.pos, accent, 2);
            return;
        }
        c.fillRect(d.source, t.color("ground").withAlpha(0.45f));
        if (d.drop == Mixer::DragView::Drop::Reorder) {
            c.fillRect(d.marker, accent);
        } else if (d.drop == Mixer::DragView::Drop::Copy) {
            c.fillRect(d.marker, accent.withAlpha(0.18f));
            c.strokeRect(d.marker.inset(0.5f), accent, 2);
        }
        if (!d.label.empty()) {
            const float size = t.type("small");
            const float w = c.textWidth(d.label, ui::Font::UiBold, size) + 16;
            const ui::Rect chip{d.pos.x + 12, d.pos.y + 10, w, kSlotH + 2};
            c.fillRound(chip, t.radius("s"), t.color("raised"));
            c.strokeRound(chip, t.radius("s"), accent);
            c.text(chip, d.label, ui::Font::UiBold, size, t.color("ink"),
                   ui::kCenter | ui::kMiddle);
        }
    }

  private:
    std::shared_ptr<const Mixer::DragView> view_;
};

}  // namespace

Mixer::Mixer(ui::Root& root, AppActions& actions, cmd::Dispatcher& dispatcher,
             engine::EngineApp& app, MeterFeed& meters)
    : root_(root),
      actions_(actions),
      dispatcher_(dispatcher),
      app_(app),
      meters_(meters),
      selectionEdit_(dispatcher, app, actions.ui()),
      trackMenu_{root,
                 dispatcher,
                 app,
                 [this](const std::string& e) {
                     if (actions_.onError) actions_.onError(e);
                 },
                 [this] { return actions_.ui().selectedTracks; },
                 [this](std::vector<std::string> tracks) {
                     actions_.ui().selectedTracks = std::move(tracks);
                 }} {
    actions_.addUi("ui.mixer", "View: mixer", [this] { toggle(); });
}

Mixer::~Mixer() {
    ghost_.reset();
    for (const Strip& s : strips_) meters_.detach(s.meter);
}

float Mixer::defaultHeight() { return panelHeight(kDefaultSlots); }

float Mixer::maxHeight() const { return maxPanelHeight(panel_, panelHeight(kMinSlots)); }

void Mixer::setHeight(float height) {
    height_ = std::clamp(height, panelHeight(kMinSlots), 4000.0f);
    applyHeight();
}

json::Value Mixer::saveLayout() const {
    json::Value v = json::Value::object();
    v["height"] = static_cast<double>(height_);
    return v;
}

void Mixer::loadLayout(const json::Value& v) {
    if (const json::Value* h = v.find("height"); h && h->isNumber() && std::isfinite(h->asDouble()))
        setHeight(static_cast<float>(h->asDouble()));
}

// The height shown: the one asked for, within what the window gives now.
void Mixer::applyHeight() {
    const float shown = std::min(height_, maxHeight());
    if (!panel_ || shown == shown_) return;
    shown_ = shown;
    panel_->layout.height = ui::SizeRule::fixed(shown_);
    root_.relayout();
    root_.damageAll();
}

void Mixer::showSelection() {
    shownSelection_ = actions_.ui().selectedTracks;
    for (const Strip& st : strips_) st.node->invalidate();
}

void Mixer::run(std::string_view action, json::Value args) {
    const actions::Action* a = actions_.registry().find(action);
    if (!a) return;
    if (auto r = actions_.run(*a, args); !r && actions_.onError)
        actions_.onError(r.error().message);
}

std::unique_ptr<ui::Node> Mixer::build() {
    auto panel = std::make_unique<ui::Node>("mixer");
    panel_ = panel.get();
    panel->layout.dir = ui::Layout::Dir::Column;
    shown_ = height_;
    panel->layout.height = ui::SizeRule::fixed(shown_);
    panel->visible = false;
    auto& grip = panel->emplace<PanelGrip>("mixer.grip", kGripH);
    grip.height = [this] { return shown_; };
    grip.resize = [this](float h) { setHeight(std::min(h, maxHeight())); };
    grip.reset = [this] { setHeight(defaultHeight()); };
    auto& header = panel->emplace<ui::Node>("mixer.header");
    header.layout.dir = ui::Layout::Dir::Row;
    header.layout.height = ui::SizeRule::fixed(kHeaderH);
    header.layout.padding = ui::Insets{4, 8, 4, 8};
    header.layout.gap = 8;
    header.layout.align = ui::Layout::Align::Center;
    header.emplace<ui::Label>("mixer.title", "Mixer", ui::Font::UiBold, "small");
    console9_ =
        &header.emplace<ui::Toggle>("mixer.console9", "Console 9", "accent", [this](bool on) {
            json::Value args = json::Value::object();
            args["console9"] = on;
            if (auto r = dispatcher_.execute({"project.set", std::move(args), kOrigin});
                !r && actions_.onError)
                actions_.onError("Console 9: " + r.error().message);
        });
    console9_->layout.width = ui::SizeRule::content();
    auto& note = header.emplace<ui::Label>("mixer.console9.note",
                                           "Subtle analog summing emulation by Airwindows",
                                           ui::Font::Ui, "caption", "ink-faint");
    note.layout.width = ui::SizeRule::fraction();
    header.emplace<ui::Button>("mixer.close", "Close", [this] { setVisible(false); });
    auto row = std::make_unique<StripRow>();
    row->onDoubleClick = [this] { addStrip("channel"); };
    // A right-click on the empty space: the TCP's "+ strip" menu.
    row->onContextMenu = [this](ui::Point p) {
        std::vector<ui::MenuItem> items;
        for (const char* type : {"Channel", "Buss", "Master"}) {
            std::string lower(type);
            for (char& ch : lower)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            items.push_back({type, {}, [this, lower] { addStrip(lower); }, true, false, false});
        }
        root_.openPopup(
            ui::makeMenu("mixer.addmenu", std::move(items), [this] { root_.closePopup(); }),
            {p.x, p.y, 1, 1});
    };
    row_ = &panel->add(std::move(row));
    return panel;
}

void Mixer::addStrip(const std::string& type) {
    if (onAddStrip) {
        onAddStrip(type);
        return;
    }
    json::Value args = json::Value::object();
    args["type"] = type;
    if (auto r = dispatcher_.execute({"track.add", std::move(args), kOrigin});
        !r && actions_.onError)
        actions_.onError(r.error().message);
}

void Mixer::toggle() { setVisible(!visible()); }

bool Mixer::visible() const { return panel_ && panel_->visible; }

void Mixer::setVisible(bool visible) {
    if (!panel_ || panel_->visible == visible) return;
    panel_->visible = visible;
    if (!visible) root_.releaseWithin(panel_);
    if (visible) {
        version_ = ~0ull;
        sync();
    }
    root_.relayout();
    root_.damageAll();
}

std::string Mixer::structure() const {
    const model::Session& s = app_.head();
    std::string out;
    std::function<void(model::Id)> walk = [&](model::Id id) {
        const model::Track* t = s.tracks.find(id);
        if (!t) return;
        out += id.str() + ":" + std::string(model::stripTypeName(t->type)) + ":" + t->name + ":" +
               t->color + ":" + (t->freeze ? "frozen:" : "");
        for (model::Id fx : t->chain) {
            const model::FxInstance* f = s.fx.find(fx);
            out += fx.str() + ",";
            if (f)
                out += std::to_string(f->bypass) + std::to_string(f->offline) + ":" +
                       std::to_string(f->label.size()) + ":" + f->label + ":" +
                       std::to_string(f->plugin.name.size()) + ":" + f->plugin.name;
        }
        for (const auto& e : s.routes)
            if (e.value->from.track == id) {
                out += e.id.str() + "," + std::to_string(e.value->mute);
                if (e.value->to.track) out += e.value->to.track->str();
                if (e.value->to.fx) out += e.value->to.fx->str();
            }
        out += ";";
        for (model::Id child : t->children) walk(child);
    };
    for (model::Id m : s.project->masters) walk(m);
    for (const auto& e : s.groups) {
        out += e.id.str() + e.value->name + e.value->color + std::to_string(e.value->active) +
               std::to_string(e.value->vca.has_value());
        for (auto id : e.value->members) out += id.str();
        for (const auto& link : e.value->links) out += link;
    }
    return out;
}

Result<json::Value> Mixer::set(const std::string& track, const char* field, json::Value value,
                               const std::string& gesture, bool bypassGroups) {
    if (projectGeneration_ != app_.projectGeneration())
        return Error("gui.mixer.stale", "the project changed; edit the control again", "/track",
                     "wait for the mixer to refresh");
    json::Value args = json::Value::object();
    args["track"] = track;
    args["bypass_groups"] = bypassGroups;
    args[field] = std::move(value);
    auto r = selectionEdit_.set(std::move(args), gesture);
    if (!r && actions_.onError) actions_.onError(std::string("mixer: ") + r.error().message);
    return r;
}

void Mixer::rebuild() {
    endDrag(false);  // the nodes it points at are about to go
    for (const Strip& s : strips_) meters_.detach(s.meter);
    strips_.clear();
    vcaStrips_.clear();
    // Remove the old strips (the row keeps its scroll bar).
    static_cast<StripRow*>(row_)->clear();
    const model::Session& s = app_.head();
    // Grouped by type: channels, busses, masters (each in track order).
    for (model::Id id : views::stripOrder(s)) {
        const model::Track& t = *s.tracks.find(id);
        const std::string tid = id.str();
        const std::string type(model::stripTypeName(t.type));
        Strip st;
        st.track = tid;
        st.type = type;
        auto frame =
            std::make_unique<StripFrame>("mixer.strip." + tid, type, [this, id](ui::Point p) {
                // Strip coordinates are the window's (the row lays them out in place).
                // On a fader, pan or button: its MIDI learn (ADR-0058).
                if (const char* param = learnableAt(id.str(), p)) {
                    openStripLearnMenu(root_, actions_, {{id}, param}, {p.x, p.y, 1, 1});
                    return;
                }
                trackMenu_.open(id, {p.x, p.y, 1, 1}, {});
            });
        st.node = frame.get();
        // The name (a double-click renames it), with "fx" (the chain) and "+"
        // (the plugin browser) at the strip's top right, as in the TCP.
        auto& nameRow = frame->emplace<ui::Node>("mixer.namerow." + tid);
        nameRow.layout.dir = ui::Layout::Dir::Row;
        nameRow.layout.height = ui::SizeRule::fixed(16);
        nameRow.layout.align = ui::Layout::Align::Center;
        nameRow.layout.gap = 2;
        auto& nameEdit = nameRow.emplace<NameEdit>(
            tid, model::trackLabel(s, id),
            [this, tid](const std::string& name) { (void)set(tid, "name", json::Value(name)); });
        st.name = nameEdit.label();
        st.rename = &nameEdit;
        st.fx = &nameRow.emplace<ui::Button>("mixer.fxadd." + tid, "fx", [this, tid] {
            json::Value args = json::Value::object();
            args["track"] = tid;
            run("ui.fx.chain", std::move(args));
        });
        st.fx->layout.width = ui::SizeRule::fixed(22);
        st.fx->layout.height = ui::SizeRule::fixed(15);
        st.addFx = &nameRow.emplace<ui::Button>("mixer.addfx." + tid, "+", [this, tid] {
            json::Value args = json::Value::object();
            args["track"] = tid;
            run("ui.fx.add", std::move(args));
        });
        st.addFx->layout.width = ui::SizeRule::fixed(16);
        st.addFx->layout.height = ui::SizeRule::fixed(15);
        frame->emplace<ColorStripe>("mixer.color." + tid, t.color);
        if (t.freeze) frame->emplace<FrozenMark>("mixer.frozen." + tid);
        // The plug-in chain (a click opens or closes the plug-in's editor); the
        // one section that takes the room the panel's height gives.
        auto& chain = frame->emplace<FxList>("mixer.fx." + tid);
        st.chain = &chain;
        chain.onDoubleClick = [this, tid] {
            json::Value args = json::Value::object();
            args["track"] = tid;
            run("ui.fx.add", std::move(args));
        };
        for (std::size_t at = 0; at < t.chain.size(); ++at) {
            const model::Id fx = t.chain[at];
            const model::FxInstance* f = s.fx.find(fx);
            if (!f) continue;
            std::string label = f->label.empty()
                                    ? (f->plugin.name.empty() ? f->plugin.id : f->plugin.name)
                                    : f->label;
            if (f->offline)
                label = "(" + label + ")";
            else if (f->bypass)
                label = "~" + label;
            auto slot = std::make_unique<FxSlot>(
                "mixer.fx." + fx.str(), label,
                [this, fx] {
                    json::Value args = json::Value::object();
                    args["fx"] = fx.str();
                    run("ui.fx.editor", std::move(args));
                },
                at);
            slot->drag.notify = [this, tid, fx, s = slot.get()](DragPhase phase, ui::Point p) {
                dragFx(tid, fx, phase, p, s->drag.mods);
            };
            slot->menu = [this, fx, s = slot.get()](ui::Point p) {
                openFxMenu(fx, s->bounds(), p);
            };
            slot->remove = [this, fx] { removeFx(fx); };
            slot->enabled = !t.freeze;
            chain.addSlot(std::move(slot));
        }
        frame->select = [this, tid](unsigned mods) {
            std::vector<std::string> shown;
            for (const Strip& x : strips_) shown.push_back(x.track);
            selectTrack(actions_.ui(), tid, mods, shown);
            const bool selected = trackSelected(actions_.ui(), tid);
            for (auto peer :
                 model::linkedTracks(app_.head(), *model::Id::parse(tid), "selection")) {
                if (!selected)
                    std::erase(actions_.ui().selectedTracks, peer.str());
                else if (!trackSelected(actions_.ui(), peer.str()))
                    actions_.ui().selectedTracks.push_back(peer.str());
            }
            const auto seeds = actions_.ui().selectedTracks;
            for (const auto& seed : seeds)
                if (auto selectedId = model::Id::parse(seed))
                    for (auto peer : model::linkedTracks(app_.head(), *selectedId, "selection"))
                        if (!trackSelected(actions_.ui(), peer.str()))
                            actions_.ui().selectedTracks.push_back(peer.str());
            showSelection();
        };
        frame->selected = [this, tid] { return trackSelected(actions_.ui(), tid); };
        // Everything but masters can be dragged (tr_0 stays first).
        if (id != model::kMaster)
            frame->drag.notify = [this, tid](DragPhase phase, ui::Point p) {
                dragStrip(tid, phase, p);
            };
        if (!s.groups.empty()) {
            const auto groups = model::trackGroups(s, id);
            auto& tabs = frame->emplace<ui::Node>("mixer.groups." + tid);
            tabs.layout.dir = ui::Layout::Dir::Row;
            tabs.layout.height = ui::SizeRule::fixed(14);
            for (auto gid : groups) {
                const auto& group = *s.groups.find(gid);
                auto& color = tabs.emplace<ColorStripe>(
                    "mixer.group.color." + tid + "." + gid.str(), group.color);
                color.layout.width = ui::SizeRule::fixed(3);
                color.layout.height = ui::SizeRule::fixed(14);
                auto& tab = tabs.emplace<ui::Button>("mixer.group." + tid + "." + gid.str(),
                                                     (group.active ? "" : "· ") + group.name);
                // The group's menu opens at its tab.
                tab.onClick = [this, gid, tid, b = &tab] {
                    trackMenu_.openGroup(gid, {tid}, b->bounds());
                };
                tab.layout.width = ui::SizeRule::fraction();
                tab.layout.height = ui::SizeRule::fixed(14);
            }
        }
        // Sends: name and level, two in view (more scroll); none leaves the
        // room to the chain. A click opens the routing matrix as "out" does,
        // Alt+click or Delete in a send's menu removes it.
        auto& sends = frame->emplace<SendList>("mixer.sends." + tid);
        for (const auto& e : s.routes) {
            if (e.value->from.track != id) continue;
            const model::Id route = e.id;
            auto row = std::make_unique<SendRow>("mixer.send." + route.str(), sendText(s, *e.value),
                                                 e.value->mute, [this, tid] {
                                                     json::Value args = json::Value::object();
                                                     args["track"] = tid;
                                                     run("ui.routing", std::move(args));
                                                 });
            row->remove = [this, route] { removeSend(route); };
            row->menu = [this, route](ui::Point p) { openSendMenu(route, p); };
            sends.addRow(std::move(row));
        }
        sends.visible = !sends.rows().empty();
        // Channels and masters (ADR-0057: a master records its output): the arm,
        // centred, under the plug-ins (busses give the room to the chain, so
        // every strip's fader lines up).
        if (t.type != model::StripType::Buss) {
            auto& rec = frame->emplace<ui::Node>("mixer.rec." + tid);
            rec.layout.dir = ui::Layout::Dir::Row;
            rec.layout.height = ui::SizeRule::fixed(20);
            rec.emplace<ui::Node>("mixer.rec.l." + tid).layout.width = ui::SizeRule::fraction();
            st.arm = &rec.emplace<ui::ArmButton>("mixer.arm." + tid, [this, tid](bool on) {
                (void)set(tid, "armed", json::Value(on));
            });
            rec.emplace<ui::Node>("mixer.rec.r." + tid).layout.width = ui::SizeRule::fraction();
        }
        // The record input and the output ("out": a click opens the routing
        // matrix, a drag onto a buss or master makes a send), as in the TCP.
        // A master's "out" picks the device outputs it plays to (ADR-0056).
        if (t.type == model::StripType::Master) {
            auto& io = frame->emplace<ui::Node>("mixer.io." + tid);
            io.layout.dir = ui::Layout::Dir::Row;
            io.layout.height = ui::SizeRule::fixed(18);
            st.out = &io.emplace<ui::Button>("mixer.out." + tid, "out");
            st.out->onClick = [this, id, button = st.out] {
                if (pickOutput) pickOutput(id, button->bounds());
            };
            st.out->layout.width = ui::SizeRule::fraction();
            st.out->layout.height = ui::SizeRule::fixed(18);
        } else {
            auto& io = frame->emplace<ui::Node>("mixer.io." + tid);
            io.layout.dir = ui::Layout::Dir::Row;
            io.layout.height = ui::SizeRule::fixed(18);
            io.layout.gap = 3;
            if (t.type == model::StripType::Channel) {
                st.input = &io.emplace<ui::Button>("mixer.input." + tid, "in \u2014");
                st.input->onClick = [this, id, button = st.input] {
                    if (pickInput) pickInput(id, button->bounds());
                };
                st.input->layout.width = ui::SizeRule::fraction();
                st.input->layout.height = ui::SizeRule::fixed(18);
            }
            auto out = std::make_unique<DragButton>("mixer.out." + tid, "out", [this, tid] {
                json::Value args = json::Value::object();
                args["track"] = tid;
                run("ui.routing", std::move(args));
            });
            out->drag.notify = [this, tid](DragPhase phase, ui::Point p) {
                dragSend(tid, phase, p);
            };
            out->layout.width = t.type == model::StripType::Channel ? ui::SizeRule::fixed(30)
                                                                    : ui::SizeRule::fraction();
            out->layout.height = ui::SizeRule::fixed(18);
            st.out = static_cast<ui::Button*>(&io.add(std::move(out)));
        }
        // Polarity, monitoring (channels) and mono in one row (ADR-0022): after
        // the chain, before the pan and the fader. Polarity and mono are the
        // TCP's small pills.
        auto& util = frame->emplace<ui::Node>("mixer.util." + tid);
        util.layout.dir = ui::Layout::Dir::Row;
        util.layout.height = ui::SizeRule::fixed(20);
        util.layout.gap = 3;
        util.layout.align = ui::Layout::Align::Center;
        st.polarity = &util.emplace<ui::Toggle>(
            "mixer.polarity." + tid, "\u00d8", "polarity",
            [this, tid](bool on) { (void)set(tid, "phase_invert", json::Value(on)); });
        st.polarity->layout.width = ui::SizeRule::fixed(18);
        if (t.type == model::StripType::Channel)
            st.monitor = &util.emplace<ui::MonitorButton>("mixer.monitor." + tid, [this, tid] {
                const auto parsed = model::Id::parse(tid);
                const model::Track* tr = parsed ? app_.head().tracks.find(*parsed) : nullptr;
                if (!tr) return;
                const char* next = tr->record.monitor == model::Monitor::Off  ? "on"
                                   : tr->record.monitor == model::Monitor::On ? "auto"
                                                                              : "off";
                (void)set(tid, "monitor", json::Value(std::string(next)));
            });
        st.mono = &util.emplace<ui::Toggle>(
            "mixer.mono." + tid, "mono", "mono",
            [this, tid](bool on) { (void)set(tid, "mono", json::Value(on)); });
        st.mono->layout.width = ui::SizeRule::fraction();
        st.polarity->layout.height = st.mono->layout.height = ui::SizeRule::fixed(14);
        st.polarity->pill = st.mono->pill = true;
        st.pan = &frame->emplace<ui::PanControl>("mixer.pan." + tid);
        st.pan->onChange = [this, tid, control = st.pan](double pan, ui::Gesture g) {
            if (g == ui::Gesture::Begin) ++gesture_;
            if (g == ui::Gesture::Begin || g == ui::Gesture::End) return;
            (void)set(
                tid, "pan", json::Value(pan),
                g == ui::Gesture::Move ? "mixer.pan:" + tid + ":" + std::to_string(gesture_) : "",
                bool(control->gestureModifiers() & ui::kAlt));
        };
        auto& faderRow = frame->emplace<ui::Node>("mixer.faderrow." + tid);
        faderRow.layout.dir = ui::Layout::Dir::Row;
        faderRow.layout.height = ui::SizeRule::fixed(kFaderH);
        faderRow.layout.gap = 4;
        auto& spacer = faderRow.emplace<ui::Node>("mixer.spacer." + tid);
        spacer.layout.width = ui::SizeRule::fraction();
        st.fader = &faderRow.emplace<ui::Fader>("mixer.fader." + tid);
        st.meter = &faderRow.emplace<ui::Meter>("mixer.meter." + tid);
        auto& spacer2 = faderRow.emplace<ui::Node>("mixer.spacer2." + tid);
        spacer2.layout.width = ui::SizeRule::fraction();
        st.fader->onChange = [this, tid, control = st.fader](double db, ui::Gesture g) {
            for (Strip& x : strips_)
                if (x.track == tid && x.value) x.value->setText(dbText(db));
            if (g == ui::Gesture::Begin) ++gesture_;
            if (g == ui::Gesture::Begin || g == ui::Gesture::End) return;
            (void)set(tid, "volume_db", json::Value(db),
                      g == ui::Gesture::Move
                          ? "mixer.volume:" + tid + ":" + std::to_string(gesture_)
                          : "",
                      bool(control->gestureModifiers() & ui::kAlt));
        };
        st.value = &frame->emplace<ui::Label>("mixer.value." + tid, dbText(t.volumeDb),
                                              ui::Font::Mono, "caption", "ink-quiet");
        st.value->align = ui::kCenter | ui::kMiddle;
        st.value->layout.height = ui::SizeRule::fixed(14);
        auto& ms = frame->emplace<ui::Node>("mixer.ms." + tid);
        ms.layout.dir = ui::Layout::Dir::Row;
        ms.layout.height = ui::SizeRule::fixed(20);
        ms.layout.gap = 3;
        st.mute = &ms.emplace<ui::Toggle>("mixer.mute." + tid, "M", "mute", [this, tid](bool on) {
            (void)set(tid, "mute", json::Value(on));
        });
        st.mute->layout.width = ui::SizeRule::fraction();
        st.solo = &ms.emplace<ui::Toggle>("mixer.solo." + tid, "S", "solo", [this, tid](bool on) {
            (void)set(tid, "solo", json::Value(std::string(on ? "on" : "off")));
        });
        st.solo->layout.width = ui::SizeRule::fraction();
        st.mute->pill = st.solo->pill = true;
        meters_.attach(tid, st.meter);
        static_cast<StripRow*>(row_)->addStrip(std::move(frame),
                                               t.type == model::StripType::Master);
        strips_.push_back(st);
    }
    for (const auto& e : s.groups) {
        if (!e.value->vca) continue;
        const auto gid = e.id;
        const auto tid = gid.str();
        Strip st;
        st.track = tid;
        st.type = "vca";
        auto frame =
            std::make_unique<StripFrame>("mixer.vca." + tid, "buss", [this, gid](ui::Point p) {
                // On the fader, mute or solo: its MIDI learn (ADR-0058).
                if (const char* param = learnableAt(gid.str(), p)) {
                    openStripLearnMenu(root_, actions_, {{gid}, param}, {p.x, p.y, 1, 1});
                    return;
                }
                trackMenu_.openGroup(gid, {}, {p.x, p.y, 1, 1});
            });
        st.node = frame.get();
        frame->emplace<ui::Label>("mixer.vca.name." + tid, e.value->name, ui::Font::UiBold, "small",
                                  "ink");
        frame->emplace<ColorStripe>("mixer.vca.color." + tid, e.value->color);
        frame->emplace<ui::Label>("mixer.vca.badge." + tid, "VCA", ui::Font::Ui, "caption",
                                  "ink-quiet");
        auto& filler = frame->emplace<ui::Node>("mixer.vca.space." + tid);
        filler.layout.height = ui::SizeRule::fraction();
        // Their menus open at the button.
        auto& members = frame->emplace<ui::Button>(
            "mixer.vca.members." + tid, std::to_string(e.value->members.size()) + " members");
        members.onClick = [this, gid, b = &members] {
            trackMenu_.openGroup(gid, actions_.ui().selectedTracks, b->bounds());
        };
        auto& env = frame->emplace<ui::Button>("mixer.vca.env." + tid, "env");
        env.onClick = [this, gid, b = &env] {
            if (envelopes) envelopes(gid, b->bounds());
        };
        st.fader = &frame->emplace<ui::Fader>("mixer.vca.fader." + tid);
        st.fader->layout.height = ui::SizeRule::fixed(kFaderH);
        st.fader->setValue(e.value->vca->volumeDb);
        auto setVca = [this, gid](const char* field, json::Value value,
                                  const std::string& gesture = {}) {
            auto args = json::Value::object();
            args["group"] = gid.str();
            args[field] = std::move(value);
            auto origin = kOrigin;
            origin.gesture = gesture;
            auto r = dispatcher_.execute({"vca.set", std::move(args), origin});
            if (!r && actions_.onError) actions_.onError(r.error().message);
        };
        st.value =
            &frame->emplace<ui::Label>("mixer.vca.value." + tid, dbText(e.value->vca->volumeDb),
                                       ui::Font::Mono, "caption", "ink-quiet");
        st.fader->onChange = [this, setVca, tid](double db, ui::Gesture g) {
            if (g == ui::Gesture::Begin) ++gesture_;
            if (g == ui::Gesture::Move || g == ui::Gesture::Set)
                setVca("volume_db", db,
                       g == ui::Gesture::Move ? "vca:" + tid + ":" + std::to_string(gesture_) : "");
        };
        auto& ms = frame->emplace<ui::Node>("mixer.vca.ms." + tid);
        ms.layout.dir = ui::Layout::Dir::Row;
        ms.layout.height = ui::SizeRule::fixed(20);
        st.mute = &ms.emplace<ui::Toggle>("mixer.vca.mute." + tid, "M", "mute",
                                          [setVca](bool on) { setVca("mute", on); });
        st.solo = &ms.emplace<ui::Toggle>("mixer.vca.solo." + tid, "S", "solo",
                                          [setVca](bool on) { setVca("solo", on); });
        st.mute->layout.width = st.solo->layout.width = ui::SizeRule::fraction();
        st.mute->pill = st.solo->pill = true;
        static_cast<StripRow*>(row_)->addStrip(std::move(frame), false);
        vcaStrips_.push_back(st);
    }
    root_.relayout();
    root_.damageAll();
}

const char* Mixer::learnableAt(const std::string& track, ui::Point p) const {
    for (const Strip& st : strips_) {
        if (st.track != track) continue;
        const std::pair<const ui::Node*, const char*> controls[] = {
            {st.fader, "volume"}, {st.pan, "pan"},        {st.mute, "mute"},
            {st.solo, "solo"},    {st.arm, "armed"},      {st.polarity, "phase_invert"},
            {st.mono, "mono"},    {st.monitor, "monitor"}};
        for (const auto& [node, param] : controls)
            if (node && node->visible && node->bounds().contains(p)) return param;
    }
    for (const Strip& st : vcaStrips_) {
        if (st.track != track) continue;
        const std::pair<const ui::Node*, const char*> controls[] = {
            {st.fader, "volume"}, {st.mute, "mute"}, {st.solo, "solo"}};
        for (const auto& [node, param] : controls)
            if (node && node->visible && node->bounds().contains(p)) return param;
    }
    return nullptr;
}

// The visible part of the strip under `p` (a strip scrolled under the pinned
// masters or past the row's edge is not there).
const Mixer::Strip* Mixer::stripAt(ui::Point p) const {
    for (const Strip& st : strips_) {
        const ui::Rect clip = st.node->parent() ? st.node->parent()->bounds() : st.node->bounds();
        if (st.node->bounds().intersect(clip).contains(p)) return &st;
    }
    return nullptr;
}

void Mixer::dragStrip(const std::string& track, DragPhase phase, ui::Point p) {
    DragView& d = *drag_;
    if (phase == DragPhase::Cancel) return endDrag(false);
    if (phase == DragPhase::Begin) {
        endDrag(false);
        d.kind = DragView::Kind::Strip;
        d.active = true;
        d.track = track;
        ghost_ = root_.addOverlay(std::make_unique<DragGhost>(drag_));
    }
    if (d.kind != DragView::Kind::Strip) return;
    d.pos = p;
    resolveStripDrop();
    if (phase == DragPhase::End) return endDrag(true);
    root_.requestAnimationFrame();
}

void Mixer::dragFx(const std::string& track, model::Id fx, DragPhase phase, ui::Point p,
                   unsigned mods) {
    DragView& d = *drag_;
    if (phase == DragPhase::Cancel) return endDrag(false);
    if (phase == DragPhase::Begin) {
        endDrag(false);
        d.kind = DragView::Kind::Fx;
        d.active = true;
        d.track = track;
        d.fx = fx.str();
        ghost_ = root_.addOverlay(std::make_unique<DragGhost>(drag_));
    }
    if (d.kind != DragView::Kind::Fx) return;
    d.pos = p;
    resolveFxDrop();
    // Ctrl onto another strip carries the whole chain; the chip says so.
    const auto id = model::Id::parse(track);
    const model::Track* t = id ? app_.head().tracks.find(*id) : nullptr;
    d.chain = (mods & ui::kCtrl) && d.drop == DragView::Drop::Copy && t;
    if (d.chain)
        d.label = "chain (" + std::to_string(t->chain.size()) + ")";
    else if (ui::Node* slot = root_.find("mixer.fx." + d.fx))
        d.label = slot->valueText();
    if (phase == DragPhase::End) return endDrag(true);
    root_.requestAnimationFrame();
}

void Mixer::dragSend(const std::string& track, DragPhase phase, ui::Point p) {
    DragView& d = *drag_;
    if (phase == DragPhase::Cancel) return endDrag(false);
    if (phase == DragPhase::Begin) {
        endDrag(false);
        d.kind = DragView::Kind::Send;
        d.active = true;
        d.track = track;
        ghost_ = root_.addOverlay(std::make_unique<DragGhost>(drag_));
    }
    if (d.kind != DragView::Kind::Send) return;
    d.pos = p;
    d.drop = DragView::Drop::None;
    for (const Strip& st : strips_)
        if (st.track == track && st.out) d.source = st.out->bounds();  // the wire's end
    // Busses and masters take sends; a channel takes only hardware input. A
    // strip the track already sends to takes no second one.
    const Strip* over = stripAt(p);
    const auto from = model::Id::parse(track),
               to = over ? model::Id::parse(over->track) : std::nullopt;
    if (over && over->track != track && over->type != "channel" && from && to &&
        !model::sendsTo(app_.head(), *from, *to)) {
        d.drop = DragView::Drop::Copy;
        d.target = over->track;
        d.marker = over->node->bounds().intersect(over->node->parent()->bounds());
    }
    if (phase == DragPhase::End) return endDrag(true);
    root_.requestAnimationFrame();
}

// A strip dropped between two strips of its own type (a master among masters):
// the index track.move takes, among its parent's children.
void Mixer::resolveStripDrop() {
    DragView& d = *drag_;
    d.drop = DragView::Drop::None;
    d.moves.clear();
    const model::Session& s = app_.head();
    const auto dragged = model::Id::parse(d.track);
    const model::Track* track = dragged ? s.tracks.find(*dragged) : nullptr;
    if (!track) return;
    std::vector<const Strip*> group;  // its type, in display order
    std::vector<model::Id> ids;
    for (const Strip& st : strips_) {
        if (st.type != std::string(model::stripTypeName(track->type))) continue;
        const auto id = model::Id::parse(st.track);
        if (!id || !s.tracks.contains(*id)) continue;  // removed before the next sync
        group.push_back(&st);
        ids.push_back(*id);
        if (st.track == d.track)
            d.source = st.node->bounds().intersect(st.node->parent()->bounds());
    }
    const ui::Rect row = row_->bounds();
    if (group.empty() || !row.contains(d.pos)) return;
    // Over a strip of another kind, or well past this kind's strips: nowhere.
    if (const Strip* over = stripAt(d.pos); over && over->type != group[0]->type) return;
    const ui::Rect first = group.front()->node->bounds(), last = group.back()->node->bounds();
    if (d.pos.x < first.x - kStripW / 2 || d.pos.x > last.right() + kStripW / 2) return;
    std::size_t gap = 0;
    for (const Strip* st : group)
        if (st->node->bounds().x + st->node->bounds().w / 2 < d.pos.x) ++gap;
    std::optional<std::size_t> index;
    if (track->type == model::StripType::Master) {
        // Among the masters, tr_0 staying first.
        const auto& masters = s.project->masters;
        std::size_t slot = 0;
        for (std::size_t i = 0; i < gap; ++i)
            if (ids[i] != *dragged) ++slot;
        slot = std::max<std::size_t>(slot, 1);
        const auto at = static_cast<std::size_t>(
            std::find(masters.begin(), masters.end(), *dragged) - masters.begin());
        if (slot != at) index = slot;
    } else {
        // In front of the first strip of its own parent and type after the gap
        // (not one moving with it), with the selected strips of that kind.
        auto selected = [this](model::Id id) { return trackSelected(actions_.ui(), id.str()); };
        const auto moving = views::reorderGroup(s, *dragged, selected);
        std::optional<model::Id> before;
        for (std::size_t i = gap; i < ids.size() && !before; ++i)
            if (std::find(moving.begin(), moving.end(), ids[i]) == moving.end() &&
                s.tracks.find(ids[i])->parent == track->parent)
                before = ids[i];
        d.moves = views::reorderMoves(s, *dragged, selected, before);
        if (!d.moves.empty()) index = 0;
    }
    if (!index) return;
    const float x = gap < group.size() ? group[gap]->node->bounds().x - 2
                                       : group.back()->node->bounds().right() + 1;
    d.marker = ui::Rect{x - 1, row.y, 3, row.h}.intersect(row);
    d.index = *index;
    d.drop = DragView::Drop::Reorder;
}

// A plug-in dropped among its strip's plug-ins (reorder) or onto another strip
// (a copy, last in that chain).
void Mixer::resolveFxDrop() {
    DragView& d = *drag_;
    d.drop = DragView::Drop::None;
    const Strip* home = nullptr;
    for (const Strip& st : strips_)
        if (st.track == d.track) home = &st;
    auto* homeList = home ? dynamic_cast<FxList*>(home->chain) : nullptr;
    if (!homeList) return;
    const FxSlot* dragged = nullptr;
    for (const FxSlot* slot : homeList->slots())
        if (slot->id() == "mixer.fx." + d.fx) dragged = slot;
    if (!dragged) return;
    d.source = dragged->bounds().intersect(homeList->bounds());
    const Strip* over = stripAt(d.pos);
    if (!over) return;
    if (over != home) {
        d.drop = DragView::Drop::Copy;
        d.target = over->track;
        d.marker = over->node->bounds().intersect(over->node->parent()->bounds());
        return;
    }
    const auto& slots = homeList->slots();
    std::size_t gap = 0;
    for (const FxSlot* slot : slots)
        if (slot->bounds().y + slot->bounds().h / 2 < d.pos.y) ++gap;
    const auto track = model::Id::parse(d.track);
    const model::Track* t = track ? app_.head().tracks.find(*track) : nullptr;
    if (!t) return;
    const std::size_t chainGap = gap < slots.size() ? slots[gap]->chainIndex : t->chain.size();
    const auto index = chainMoveIndex(dragged->chainIndex, chainGap);
    if (!index) return;
    const float y =
        gap < slots.size() ? slots[gap]->bounds().y - 2 : slots.back()->bounds().bottom() + 0.5f;
    d.marker = ui::Rect{homeList->bounds().x, y - 0.5f, homeList->bounds().w, 3}.intersect(
        homeList->bounds());
    d.index = *index;
    d.drop = DragView::Drop::Reorder;
}

// A plug-in's own menu (a right-click on it in the chain).
void Mixer::openFxMenu(model::Id fx, const ui::Rect& slot, ui::Point at) {
    if (!app_.head().fx.find(fx)) return;
    std::vector<ui::MenuItem> items(3);
    items[0].label = "Rename…";
    items[0].run = [this, fx, slot] { openFxRename(fx, slot); };
    items[1].label = "Duplicate";
    items[1].run = [this, fx] { duplicateFx(fx); };
    items[2].label = "Delete";
    items[2].run = [this, fx] { removeFx(fx); };
    root_.openPopup(
        ui::makeMenu("mixer.fxmenu." + fx.str(), std::move(items), [this] { root_.closePopup(); }),
        ui::Rect{at.x, at.y, 0, 0}, ui::Placement::Below);
}

// A copy of the plug-in with its live settings, right after it in its chain
// (fx.copy, one undo step).
void Mixer::duplicateFx(model::Id fx) {
    const auto& s = app_.head();
    const auto track = model::trackOfFx(s, fx);
    const model::Track* t = track ? s.tracks.find(*track) : nullptr;
    if (!t) return;
    const auto at = std::find(t->chain.begin(), t->chain.end(), fx);
    if (at == t->chain.end()) return;  // an input-chain plug-in: not shown here
    json::Value args = json::Value::object();
    args["fx"] = fx.str();
    args["track"] = track->str();
    args["index"] = static_cast<std::int64_t>(at - t->chain.begin() + 1);
    if (auto r = dispatcher_.execute({"fx.copy", std::move(args), kOrigin}); !r && actions_.onError)
        actions_.onError("mixer: " + r.error().message);
}

// Alt+click on a plug-in, or Delete in its menu (one undo step).
void Mixer::removeFx(model::Id fx) {
    json::Value args = json::Value::object();
    args["fx"] = fx.str();
    if (auto r = dispatcher_.execute({"fx.remove", std::move(args), kOrigin});
        !r && actions_.onError)
        actions_.onError("mixer: " + r.error().message);
}

// A send's own menu (a right-click on it under the chain).
void Mixer::openSendMenu(model::Id route, ui::Point at) {
    if (!app_.head().routes.find(route)) return;
    std::vector<ui::MenuItem> items(1);
    items[0].label = "Delete";
    items[0].run = [this, route] { removeSend(route); };
    root_.openPopup(ui::makeMenu("mixer.sendmenu." + route.str(), std::move(items),
                                 [this] { root_.closePopup(); }),
                    ui::Rect{at.x, at.y, 0, 0}, ui::Placement::Below);
}

// Alt+click on a send, or Delete in its menu (one undo step).
void Mixer::removeSend(model::Id route) {
    json::Value args = json::Value::object();
    args["route"] = route.str();
    if (auto r = dispatcher_.execute({"route.remove", std::move(args), kOrigin});
        !r && actions_.onError)
        actions_.onError("mixer: " + r.error().message);
}

// A name for this instance over its slot; an empty one goes back to the
// plug-in's own name (fx.set label).
void Mixer::openFxRename(model::Id fx, const ui::Rect& slot) {
    const model::FxInstance* f = app_.head().fx.find(fx);
    if (!f) return;
    const std::string shown = f->label.empty() ? f->plugin.name : f->label;
    auto field = std::make_unique<ui::TextField>("mixer.fxrename." + fx.str(), shown);
    ui::TextField* edit = field.get();
    edit->layout.width = ui::SizeRule::fixed(std::max(slot.w, 160.0f));
    edit->layout.height = ui::SizeRule::fixed(24);
    edit->onSubmit = [this, fx](const std::string& text) {
        json::Value args = json::Value::object();
        args["fx"] = fx.str();
        args["label"] = text;  // a copy: closing destroys the field and its text
        ui::Root& r = root_;
        if (auto done = dispatcher_.execute({"fx.set", std::move(args), kOrigin});
            !done && actions_.onError)
            actions_.onError("mixer: " + done.error().message);
        r.closePopup();  // last: it destroys this lambda's field
    };
    edit->onCancel = [this] { root_.closePopup(); };
    root_.openPopup(std::move(field), ui::Rect{slot.x, slot.y, slot.w, 0});
    edit->selectAll();
    root_.focus(edit);
}

// The drag is over: drop it (one command, one undo step) or let it go.
void Mixer::endDrag(bool commit) {
    const DragView done = *drag_;
    *drag_ = DragView{};
    ghost_.reset();
    if (done.kind == DragView::Kind::None) return;
    root_.requestAnimationFrame();
    if (!commit || done.drop == DragView::Drop::None) return;
    json::Value args = json::Value::object();
    const char* command = "fx.move";
    if (done.kind == DragView::Kind::Send) {
        command = "route.add";
        args["from"] = done.track;
        args["to"] = done.target;
    } else if (done.kind == DragView::Kind::Strip && !done.moves.empty()) {
        auto r = dispatcher_.transact(
            done.moves.size() == 1 ? "Move track" : "Move tracks", kOrigin,
            [&]() -> Result<json::Value> {
                for (const views::TrackMove& m : done.moves) {
                    json::Value one = json::Value::object();
                    one["track"] = m.track.str();
                    one["index"] = static_cast<std::int64_t>(m.index);
                    SUPPLE_TRY(dispatcher_.execute({"track.move", std::move(one), kOrigin}));
                }
                return json::Value::object();
            });
        if (!r && actions_.onError) actions_.onError(std::string("mixer: ") + r.error().message);
        return;
    } else if (done.kind == DragView::Kind::Strip) {
        command = "track.move";
        args["track"] = done.track;
        args["index"] = static_cast<std::int64_t>(done.index);
    } else if (done.drop == DragView::Drop::Copy && done.chain) {
        // The whole chain, in order, after the target's own plug-ins.
        const auto id = model::Id::parse(done.track);
        const model::Track* t = id ? app_.head().tracks.find(*id) : nullptr;
        const std::vector<model::Id> chain = t ? t->chain : std::vector<model::Id>{};
        auto r = dispatcher_.transact("Copy FX chain", kOrigin, [&]() -> Result<json::Value> {
            for (const model::Id fx : chain) {
                json::Value one = json::Value::object();
                one["fx"] = fx.str();
                one["track"] = done.target;
                SUPPLE_TRY(dispatcher_.execute({"fx.copy", std::move(one), kOrigin}));
            }
            return json::Value::object();
        });
        if (!r && actions_.onError) actions_.onError("mixer: " + r.error().message);
        return;
    } else if (done.drop == DragView::Drop::Copy) {
        command = "fx.copy";
        args["fx"] = done.fx;
        args["track"] = done.target;
    } else {
        args["fx"] = done.fx;
        args["index"] = static_cast<std::int64_t>(done.index);
    }
    if (auto r = dispatcher_.execute({command, std::move(args), kOrigin}); !r && actions_.onError)
        actions_.onError(std::string("mixer: ") + r.error().message);
}

void Mixer::update() {
    const model::Session& s = app_.head();
    if (console9_) console9_->setOn(s.project->console9);
    for (auto& st : vcaStrips_) {
        const auto id = model::Id::parse(st.track);
        const auto* g = id ? s.groups.find(*id) : nullptr;
        if (!g || !g->vca) continue;
        if (!st.fader->dragging()) {
            st.fader->setValue(g->vca->volumeDb);
            st.value->setText(dbText(g->vca->volumeDb));
        }
        st.mute->setOn(g->vca->mute);
        st.solo->setOn(g->vca->solo);
    }
    for (Strip& st : strips_) {
        const auto id = model::Id::parse(st.track);
        const model::Track* t = id ? s.tracks.find(*id) : nullptr;
        if (!t) continue;
        if (!st.fader->dragging()) {
            st.fader->setValue(t->volumeDb);
            st.value->setText(dbText(t->volumeDb));
        }
        for (const auto& route : s.routes) {
            if (route.value->from.track != *id) continue;
            auto* row = dynamic_cast<SendRow*>(st.node->find("mixer.send." + route.id.str()));
            if (!row) continue;
            row->setText(sendText(s, *route.value));
        }
        st.pan->setValue(t->pan);
        st.mute->setOn(t->mute);
        st.solo->setOn(t->solo != model::Solo::Off);
        st.polarity->enabled = st.mono->enabled = !t->freeze;
        st.addFx->enabled = !t->freeze;
        if (st.arm) st.arm->enabled = !t->freeze;
        if (st.monitor) st.monitor->enabled = !t->freeze;
        st.polarity->setOn(t->phaseInvert);
        st.mono->setOn(t->mono);
        if (st.arm) st.arm->setOn(t->record.armed);
        if (st.monitor)
            st.monitor->setMode(
                t->record.monitor == model::Monitor::Off  ? ui::MonitorButton::Mode::Off
                : t->record.monitor == model::Monitor::On ? ui::MonitorButton::Mode::On
                                                          : ui::MonitorButton::Mode::Auto);
        if (st.input && inputText) st.input->setText(inputText(*id));
        if (t->type == model::StripType::Master && st.out && outputText)
            st.out->setText(outputText(*id));
    }
}

void Mixer::sync() {
    if (!visible()) return;
    applyHeight();  // the window may have been resized
    if (actions_.ui().selectedTracks != shownSelection_) showSelection();  // the TCP changed it
    const bool replaced = app_.projectGeneration() != projectGeneration_;
    if (!replaced && app_.head().version == version_) return;
    projectGeneration_ = app_.projectGeneration();
    version_ = app_.head().version;
    const std::string now = structure();
    if (replaced || now != structure_) {
        structure_ = now;
        rebuild();
    }
    update();
}

}  // namespace supple::gui
