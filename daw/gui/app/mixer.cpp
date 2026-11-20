// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "mixer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <utility>

#include <SDL3/SDL_keycode.h>

#include "pan_menu.h"
#include "panel_grip.h"
#include "panel_height.h"
#include "param_menu.h"
#include "rack_controls.h"
#include "rack_engine.h"
#include "strip_learn.h"
#include "supple/model/groups.h"
#include "supple/ui/controls.h"
#include "supple/ui/list.h"
#include "supple/ui/popup.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"
#include "supple/views/control_names.h"
#include "supple/views/track_layout.h"

namespace supple::gui {
namespace {

const cmd::Origin kOrigin{cmd::Origin::Type::Ui, "gui", {}};
constexpr float kStripW = 92;
// The strip's chain section is the only one that follows the panel's height;
// the rest (name, sends, in/out, polarity/mono, pan, the state row, fader,
// values, gaps, padding) is fixed.
constexpr float kSlotH = 18, kSlotGap = 2;
constexpr float kGripH = 5, kHeaderH = 30;
constexpr float kFaderH = 133;  // the fader and meter row
// A strip is a grid of four columns (gui.md §4.4): a row is four cells, two
// halves or one full width, every button kCellH high.
constexpr float kCellH = 16, kCellGap = 3;
constexpr float kColourBar = 3;  // the track's colour, across the strip's top
constexpr float kFxRowH = 14;    // "fx" and "+", over the plug-ins
// The fixed rows of a strip with no sends, their gaps and the padding (each
// send adds kSendH): the same for every type, so every chain is as tall.
constexpr float kStripChrome = 152;
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
    // What the plug-in is doing: a bypassed one's name is dimmed and struck
    // through; an offline (unloaded) one's is dimmed inside a dashed outline. A disabled one (this
    // run does not load it) or a failed one (its process closed or stopped answering) keeps the
    // solid outline and its place and size, dims its name and carries a bar down the left edge,
    // inside the outline: amber for disabled, `clip` red for failed (docs/specs/gui.md, "Disabled
    // plug-ins are marked"). Disabled and failed win over offline and bypassed: they are the ones
    // that need the person.
    enum class State { On, Bypassed, Offline, Disabled, Failed };
    State state = State::On;
    std::string valueText() const override {
        const std::string name = Button::valueText();
        return state == State::Disabled   ? name + " (disabled)"
               : state == State::Failed   ? name + " (failed)"
               : state == State::Bypassed ? name + " (bypassed)"
               : state == State::Offline  ? name + " (offline)"
                                          : name;
    }
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        const ui::Rect b = bounds();
        const ui::Color face = !enabled    ? t.color("panel")
                               : pressed() ? t.color("selection")
                               : hovered() ? t.color("raised")
                                           : t.color("panel");
        c.fillRound(b, t.radius("s"), face);
        if (state == State::Offline && !focused()) {
            // Dashes, three units on and three off, along each edge.
            const ui::Color ink = t.color("ink-faint");
            for (float x = b.x + 2; x < b.right() - 2; x += 6) {
                const float end = std::min(x + 3, b.right() - 2);
                c.line({x, b.y + 0.5f}, {end, b.y + 0.5f}, ink);
                c.line({x, b.bottom() - 0.5f}, {end, b.bottom() - 0.5f}, ink);
            }
            for (float y = b.y + 2; y < b.bottom() - 2; y += 6) {
                const float end = std::min(y + 3, b.bottom() - 2);
                c.line({b.x + 0.5f, y}, {b.x + 0.5f, end}, ink);
                c.line({b.right() - 0.5f, y}, {b.right() - 0.5f, end}, ink);
            }
        } else {
            c.strokeRound(b, t.radius("s"), focused() ? t.color("focus") : t.color("rule"));
        }
        const bool needsYou = state == State::Disabled || state == State::Failed;
        if (needsYou) {
            const float inset = std::max(2.0f, t.radius("s"));
            c.fillRect({b.x + 1, b.y + inset, 2.0f, b.h - 2 * inset},
                       t.color(state == State::Failed ? "meter-clip" : "meter-hot"));
        }
        const std::string name = Button::valueText();
        const float size = t.type("small");
        c.text(b, name, ui::Font::UiBold, size,
               t.color(needsYou                        ? "ink-quiet"
                       : enabled && state == State::On ? "ink"
                                                       : "ink-faint"),
               ui::kCenter | ui::kMiddle);
        if (state == State::Bypassed) {
            const float w = std::min(b.w - 4, c.textWidth(name, ui::Font::UiBold, size));
            const float mid = b.x + b.w / 2, y = std::round(b.y + b.h / 2) + 0.5f;
            c.line({mid - w / 2, y}, {mid + w / 2, y}, t.color("ink-quiet"));
        }
    }
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

// A rack that feeds a channel, above its plug-ins (signal order): its name on
// a rack module's face, and a faint "rack". A modulator rack (one homed on the
// strip's track that holds a Mod Out, ADR-0096) is a mod slot: the same face
// with "mod" in signal-cv, the colour modulation has everywhere. A click opens
// the rack window on it (a double-click's second press does nothing more); a
// right-click is the strip's.
class RackSlot : public ui::Button {
  public:
    RackSlot(std::string id, std::string name, std::function<void()> click, bool mod = false)
        : Button(std::move(id), name, std::move(click)), name_(std::move(name)), mod_(mod) {}
    bool mod() const { return mod_; }
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.button == 1 && e.clicks >= 2 &&
            (e.type == ui::PointerEvent::Type::Down || e.type == ui::PointerEvent::Type::Up))
            return true;
        return Button::onPointer(e);
    }
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        const ui::Rect b = bounds();
        c.fillRound(b, t.radius("s"),
                    pressed()   ? t.color("selection")
                    : hovered() ? t.color("raised")
                                : t.color("module"));
        c.strokeRound(b, t.radius("s"), t.color("rule"));
        const float size = t.type("small"), tagSize = t.type("caption");
        const char* tagText = mod_ ? "mod" : "rack";
        const float tagW = c.textWidth(tagText, ui::Font::Ui, tagSize);
        const float nameW = c.textWidth(name_, ui::Font::UiBold, size);
        // A rack's tag only where the name keeps its room; a mod slot's always
        // (the name is cut short instead), since "mod" is what marks it.
        const bool tag = mod_ || nameW + tagW + 14 <= b.w;
        const ui::Rect text{b.x + 4, b.y, b.w - 8 - (tag ? tagW + 4 : 0), b.h};
        c.text(text, name_, ui::Font::UiBold, size, t.color("ink"),
               (tag ? ui::kLeft : ui::kCenter) | ui::kMiddle);
        if (tag)
            c.text({b.right() - 4 - tagW, b.y, tagW, b.h}, tagText, ui::Font::Ui, tagSize,
                   t.color(mod_ ? "signal-cv" : "ink-faint"), ui::kLeft | ui::kMiddle);
    }

  private:
    std::string name_;
    bool mod_ = false;
};

// The dB marks of a strip's meter, beside it: 0, then how far under it.
class MeterScale : public ui::Node {
  public:
    // The levels it labels, in dB below full scale. Each has a tick that ends
    // at the scale's right edge, where the meter starts, and the meter draws
    // the same levels as lines across its bars (ui::Meter::setMarks).
    static std::vector<float> marks() { return {0, -12, -24, -36, -48}; }
    static constexpr float kTick = 4, kWidth = 24;
    explicit MeterScale(std::string id) : Node(std::move(id)) {
        layout.width = ui::SizeRule::fixed(kWidth);
        layout.height = ui::SizeRule::fraction();
    }
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        const ui::Rect b = bounds();
        const float top = b.y + ui::Meter::kClipLight, lane = b.h - ui::Meter::kClipLight;
        for (float db : marks()) {
            const float y = std::round(top + (1 - ui::Meter::position(db)) * lane);
            const bool zero = db == 0;
            c.fillRect({b.right() - kTick, y, kTick, 1}, t.color(zero ? "ink-quiet" : "ink-faint"));
            c.text({b.x, y - 6, b.w - kTick - 2, 12}, std::to_string(static_cast<int>(-db)),
                   ui::Font::Mono, t.type("caption"), t.color(zero ? "ink-quiet" : "ink-faint"),
                   ui::kRight | ui::kMiddle);
        }
    }
};

// The highest peak a strip's meter has shown, in dB, under the meter: empty
// until there is one, in the clip colour over 0 dB. A click clears it, as a
// click on the meter does.
class PeakReadout : public ui::Node {
  public:
    PeakReadout(std::string id, ui::Meter& meter) : Node(std::move(id), ui::Role::Label) {
        interactive = true;
        meter_ = &meter;
        meter.onPeak = [this] { invalidate(); };
    }
    std::string valueText() const override {
        const float db = meter_->maxPeakDb();
        if (db <= ui::Meter::kNoPeak) return {};
        char buf[24];
        std::snprintf(buf, sizeof buf, "%+.1f", static_cast<double>(db));
        return buf;
    }
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        c.text(bounds(), valueText(), ui::Font::Mono, t.type("caption"),
               t.color(meter_->maxPeakDb() > 0 ? "meter-clip" : "ink-quiet"),
               ui::kCenter | ui::kMiddle);
    }
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.type != ui::PointerEvent::Type::Down || e.button != 1) return false;
        meter_->clearPeak();
        return true;
    }

  private:
    ui::Meter* meter_;
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
    // A rack that feeds the strip: above the plug-ins, in route order.
    RackSlot& addRack(std::unique_ptr<RackSlot> slot) {
        RackSlot& ref = *slot;
        racks_.push_back(&ref);
        add(std::move(slot));
        return ref;
    }
    const std::vector<FxSlot*>& slots() const { return slots_; }
    const std::vector<RackSlot*>& racks() const { return racks_; }
    float content() const { return chainRows(static_cast<int>(racks_.size() + slots_.size())); }
    // Where the list is scrolled to; a rebuilt strip's list takes the old
    // one's place (arrange clamps it to the new content).
    float scroll() const { return scroll_; }
    void setScroll(float v) { scroll_ = v; }
    // A scroll by `dy` units (the edge scroll): true when the list moved.
    bool scrollBy(float dy) {
        const float limit = std::max(0.0f, content() - bounds().h);
        const float next = std::clamp(scroll_ + dy, 0.0f, limit);
        if (next == scroll_) return false;
        scroll_ = next;
        relayout();
        invalidate();
        return true;
    }
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
        for (RackSlot* slot : racks_) {
            slot->arrange(canvas, {b.x, y, w, kSlotH});
            y += kSlotH + kSlotGap;
        }
        for (FxSlot* slot : slots_) {
            slot->arrange(canvas, {b.x, y, w, kSlotH});
            y += kSlotH + kSlotGap;
        }
    }
    std::function<void()> onDoubleClick;  // on the list's empty part: add a plug-in
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.type == ui::PointerEvent::Type::Down && e.button == 1 && e.clicks >= 2 &&
            onDoubleClick &&
            std::none_of(slots_.begin(), slots_.end(),
                         [&](const FxSlot* slot) { return slot->bounds().contains(e.pos); }) &&
            std::none_of(racks_.begin(), racks_.end(),
                         [&](const RackSlot* slot) { return slot->bounds().contains(e.pos); })) {
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
    std::vector<RackSlot*> racks_;
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
    // The part of the row that scrolls (the masters at its right do not),
    // and a scroll by `dx` units: true when the strips moved.
    ui::Rect scrolledArea() const { return scrolled_->bounds(); }
    ui::Rect pinnedArea() const { return pinned_->bounds(); }
    bool scrollBy(float dx) {
        const float limit = std::max(0.0f, scrolled_->content() - scrolled_->bounds().w);
        const float next = std::clamp(scrolled_->scroll + dx, 0.0f, limit);
        if (next == scrolled_->scroll) return false;
        scrolled_->scroll = next;
        relayout();
        invalidate();
        return true;
    }
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
        layout.height = ui::SizeRule::fixed(kColourBar);
    }
    void paint(ui::Canvas& c) override {
        if (const auto colour = c.theme().trackColor(colour_)) c.fillRect(bounds(), *colour);
    }

  private:
    std::string colour_;
};

// A frozen channel's mark: a snowflake before the strip's name, as in the
// track panel.
class FrozenMark : public ui::Node {
  public:
    explicit FrozenMark(std::string id) : Node(std::move(id)) {
        layout.width = ui::SizeRule::fixed(13);
        layout.height = ui::SizeRule::fixed(14);
    }
    void paint(ui::Canvas& c) override {
        const ui::Rect b = bounds();
        ui::drawSnowflake(c, {b.x + 6, b.y + b.h / 2}, 6.0f, c.theme().color("ink"));
    }
};

// A hairline that takes the room between two things in a row.
class RuleLine : public ui::Node {
  public:
    explicit RuleLine(std::string id) : Node(std::move(id)) {
        layout.width = ui::SizeRule::fraction();
        layout.height = ui::SizeRule::fixed(1);
    }
    void paint(ui::Canvas& c) override { c.fillRect(bounds(), c.theme().color("rule")); }
};

// A buss's or master's mark before its name (ui::drawTrackGlyph); a channel
// has none.
class TypeMark : public ui::Node {
  public:
    TypeMark(std::string id, ui::TrackGlyph glyph) : Node(std::move(id)), glyph_(glyph) {
        layout.width = ui::SizeRule::fixed(13);
        layout.height = ui::SizeRule::fixed(14);
    }
    void paint(ui::Canvas& c) override {
        const ui::Rect b = bounds();
        ui::drawTrackGlyph(c, {b.x, b.y, 10, b.h}, glyph_, c.theme().color("ink-quiet"));
    }
    std::string valueText() const override {
        return glyph_ == ui::TrackGlyph::Buss ? "buss" : "master";
    }

  private:
    ui::TrackGlyph glyph_;
};

// A strip's background; a right-click anywhere on it opens the track menu,
// and a drag from a part no control uses moves the strip among its own kind.
// Its first child is the colour bar across its top (the track's own colour:
// the type is a mark before the name, not a colour).
class StripFrame : public ui::Node {
  public:
    StripFrame(std::string id, std::function<void(ui::Point)> menu)
        : Node(std::move(id)), menu_(std::move(menu)) {
        interactive = true;
        layout.dir = ui::Layout::Dir::Column;
        layout.padding = ui::Insets{5, 0, 5, 5};
        layout.gap = 4;
    }
    DragTracker drag;  // notify unset: this strip cannot be dragged
    // The track selection the TCP shares (gui.md §4.2): a press on the strip's
    // empty part selects (Ctrl toggles, Shift extends); a right-click keeps a
    // selection the strip is part of.
    std::function<void(unsigned mods)> select;
    std::function<bool()> selected;
    void arrange(const ui::Canvas& canvas, const ui::Rect& r) override {
        Node::arrange(canvas, r);
        // The colour bar runs the strip's whole width, over the padding.
        if (!children().empty()) {
            ui::Node& bar = *children().front();
            bar.arrange(canvas, {r.x, r.y, r.w - 1, bar.bounds().h});
        }
    }
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        c.fillRect(bounds(), t.color("panel"));
        if (selected && selected()) {  // highlighted and outlined, as in the TCP
            c.fillRect(bounds(), t.color("selection").withAlpha(0.55f));
            c.strokeRect({bounds().x + 0.5f, bounds().y + 0.5f, bounds().w - 1, bounds().h - 1},
                         t.color("focus"), 1.0f);
        }
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
                 },
                 [this](const char* id, const model::Id& track) {
                     const actions::Action* a = actions_.registry().find(id);
                     if (!a) return;
                     json::Value args = json::Value::object();
                     args["track"] = track.str();
                     if (auto r = actions_.run(*a, args); !r && actions_.onError)
                         actions_.onError(r.error().message);
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
    // A right-click on the empty space: the TCP's "+ track" menu.
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
        follow();
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
                out += std::to_string(f->bypass) + std::to_string(f->offline) +
                       (attention_.markOf(fx, *f) == FxAttention::Mark::Disabled ? "D"
                        : attention_.markOf(fx, *f) == FxAttention::Mark::Failed ? "F"
                                                                                 : "") +
                       ":" + std::to_string(f->label.size()) + ":" + f->label + ":" +
                       std::to_string(f->plugin.name.size()) + ":" + f->plugin.name;
        }
        for (model::Id rack : racks::racksFeeding(s, id))
            out += rack.str() + "," + model::rackLabel(s, rack) + ",";
        out += "m";
        for (model::Id rack : racks::modRacks(s, id))
            out += rack.str() + "," + model::rackLabel(s, rack) + ",";
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

void Mixer::rebuild(bool keepLists) {
    endDrag(false);  // the nodes it points at are about to go
    // A strip's plug-in list stays where it is scrolled to (a plug-in dropped
    // at the end of a long chain is still in view after it); a new project's
    // lists start at their tops.
    std::vector<std::pair<std::string, float>> listScroll;
    if (keepLists)
        for (const Strip& st : strips_)
            if (auto* list = dynamic_cast<FxList*>(st.chain); list && list->scroll() > 0)
                listScroll.emplace_back(st.track, list->scroll());
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
        auto frame = std::make_unique<StripFrame>("mixer.strip." + tid, [this, id](ui::Point p) {
            // Strip coordinates are the window's (the row lays them out in place).
            // On a fader, pan or button: the parameter menu (MIDI learn, lane,
            // reset; a pan's mode and law), as in the track panel.
            if (const char* param = learnableAt(id.str(), p)) {
                openParamMenu(root_, actions_, id, param, {p.x, p.y, 1, 1});
                return;
            }
            trackMenu_.open(id, {p.x, p.y, 1, 1}, {});
        });
        st.node = frame.get();
        // The track's colour across the top, then the header: the type's mark
        // (a buss or a master), a frozen track's snowflake and the name, which
        // has the strip's width to itself (a double-click renames it).
        frame->emplace<ColorStripe>("mixer.color." + tid, t.color);
        auto& nameRow = frame->emplace<ui::Node>("mixer.namerow." + tid);
        nameRow.layout.dir = ui::Layout::Dir::Row;
        nameRow.layout.height = ui::SizeRule::fixed(16);
        nameRow.layout.align = ui::Layout::Align::Center;
        nameRow.layout.gap = 2;
        if (t.type != model::StripType::Channel)
            nameRow.emplace<TypeMark>("mixer.type." + tid, t.type == model::StripType::Buss
                                                               ? ui::TrackGlyph::Buss
                                                               : ui::TrackGlyph::Master);
        if (t.freeze) nameRow.emplace<FrozenMark>("mixer.frozen." + tid);
        auto& nameEdit = nameRow.emplace<NameEdit>(
            tid, model::trackLabel(s, id),
            [this, tid](const std::string& name) { (void)set(tid, "name", json::Value(name)); });
        st.name = nameEdit.label();
        st.rename = &nameEdit;
        // The plug-in list's own line: "fx" (the chain window) at its left
        // and "+" (the plugin browser) at its right, a rule between them.
        auto& fxRow = frame->emplace<ui::Node>("mixer.fxrow." + tid);
        fxRow.layout.dir = ui::Layout::Dir::Row;
        fxRow.layout.height = ui::SizeRule::fixed(kFxRowH);
        fxRow.layout.align = ui::Layout::Align::Center;
        fxRow.layout.gap = 4;
        st.fx = &fxRow.emplace<ui::Button>("mixer.fxadd." + tid, "fx", [this, tid] {
            json::Value args = json::Value::object();
            args["track"] = tid;
            run("ui.fx.chain", std::move(args));
        });
        st.fx->layout.width = ui::SizeRule::fixed(22);
        st.fx->layout.height = ui::SizeRule::fixed(kFxRowH);
        fxRow.emplace<RuleLine>("mixer.fxrule." + tid);
        st.addFx = &fxRow.emplace<ui::Button>("mixer.addfx." + tid, "+", [this, tid] {
            json::Value args = json::Value::object();
            args["track"] = tid;
            run("ui.fx.add", std::move(args));
        });
        st.addFx->layout.width = ui::SizeRule::fixed(16);
        st.addFx->layout.height = ui::SizeRule::fixed(kFxRowH);
        // As in the TCP: text alone at rest, "fx" in full ink over a chain.
        st.fx->quiet = st.addFx->quiet = true;
        st.fx->setEmphasis(!t.chain.empty() || !t.inputChain.empty());
        // The plug-in chain (a click opens or closes the plug-in's editor); the
        // one section that takes the room the panel's height gives.
        auto& chain = frame->emplace<FxList>("mixer.fx." + tid);
        st.chain = &chain;
        for (const auto& [track, scroll] : listScroll)
            if (track == tid) chain.setScroll(scroll);
        chain.onDoubleClick = [this, tid] {
            json::Value args = json::Value::object();
            args["track"] = tid;
            run("ui.fx.add", std::move(args));
        };
        // The racks that feed it (their Outs route to its input), first, then
        // its modulator racks (mod slots). A modulator that also feeds it
        // shows once, where it feeds, as a mod slot.
        const std::vector<model::Id> modulators = racks::modRacks(s, id);
        auto isMod = [&](model::Id rack) {
            return std::find(modulators.begin(), modulators.end(), rack) != modulators.end();
        };
        std::vector<model::Id> slotted;
        auto addRackSlot = [&](model::Id rack) {
            const bool mod = isMod(rack);
            auto slot = std::make_unique<RackSlot>(
                std::string(mod ? "mixer.mod." : "mixer.rack.") + tid + "." + rack.str(),
                model::rackLabel(s, rack),
                [this, rack] {
                    if (openRack) openRack(rack);
                },
                mod);
            st.racks.push_back(&chain.addRack(std::move(slot)));
            slotted.push_back(rack);
        };
        if (t.type == model::StripType::Channel)
            for (model::Id rack : racks::racksFeeding(s, id)) addRackSlot(rack);
        for (model::Id rack : modulators)
            if (std::find(slotted.begin(), slotted.end(), rack) == slotted.end()) addRackSlot(rack);
        for (std::size_t at = 0; at < t.chain.size(); ++at) {
            const model::Id fx = t.chain[at];
            const model::FxInstance* f = s.fx.find(fx);
            if (!f) continue;
            std::string label = f->label.empty()
                                    ? (f->plugin.name.empty() ? f->plugin.id : f->plugin.name)
                                    : f->label;
            auto slot = std::make_unique<FxSlot>(
                "mixer.fx." + fx.str(), label,
                [this, fx, label] {
                    // A plug-in that is not loaded, or whose process failed, has no editor to
                    // open: say so instead.
                    const model::FxInstance* now = app_.head().fx.find(fx);
                    const auto mark = now ? attention_.markOf(fx, *now) : FxAttention::Mark::None;
                    if (mark == FxAttention::Mark::Disabled) {
                        if (actions_.onError)
                            actions_.onError(label +
                                             " is disabled for this run and not loaded. "
                                             "Right-click it to enable it.");
                        return;
                    }
                    if (mark == FxAttention::Mark::Failed) {
                        if (actions_.onError)
                            actions_.onError(label +
                                             " failed: its separate process closed or stopped "
                                             "answering. Restart it in the FX window.");
                        return;
                    }
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
            switch (attention_.markOf(fx, *f)) {
                case FxAttention::Mark::Disabled:
                    slot->state = FxSlot::State::Disabled;
                    break;
                case FxAttention::Mark::Failed:
                    slot->state = FxSlot::State::Failed;
                    break;
                case FxAttention::Mark::None:
                    slot->state = f->offline  ? FxSlot::State::Offline
                                  : f->bypass ? FxSlot::State::Bypassed
                                              : FxSlot::State::On;
                    break;
            }
            chain.addSlot(std::move(slot));
        }
        // One rule for the warning colour of "fx", here and in the track list: the chain holds a
        // plug-in that is disabled or failed.
        st.fx->setTextColor(attention_.chainNeeds(s, t) ? "meter-hot" : "");
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
        // Routing, where the signal enters and leaves: the record input
        // (channels) and the output ("out": a click opens the routing matrix,
        // a drag onto a buss or master makes a send), as in the TCP. A
        // master's "out" picks the device outputs it plays to (ADR-0056).
        auto& io = frame->emplace<ui::Node>("mixer.io." + tid);
        io.layout.dir = ui::Layout::Dir::Row;
        io.layout.height = ui::SizeRule::fixed(kCellH);
        io.layout.gap = kCellGap;
        if (t.type == model::StripType::Master) {
            st.out = &io.emplace<ui::Button>("mixer.out." + tid, "out");
            st.out->onClick = [this, id, button = st.out] {
                if (pickOutput) pickOutput(id, button->bounds());
            };
            st.out->layout.width = ui::SizeRule::fraction();
        } else {
            if (t.type == model::StripType::Channel) {
                st.input = &io.emplace<ui::Button>("mixer.input." + tid, "in \u2014");
                st.input->onClick = [this, id, button = st.input] {
                    if (pickInput) pickInput(id, button->bounds());
                };
                st.input->layout.width = ui::SizeRule::fraction();
                st.input->layout.height = ui::SizeRule::fixed(kCellH);
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
            st.out = static_cast<ui::Button*>(&io.add(std::move(out)));
        }
        st.out->layout.height = ui::SizeRule::fixed(kCellH);
        // Polarity and mono (ADR-0022), two halves: they act after the chain
        // and before the pan, so they sit on it.
        auto& util = frame->emplace<ui::Node>("mixer.util." + tid);
        util.layout.dir = ui::Layout::Dir::Row;
        util.layout.height = ui::SizeRule::fixed(kCellH);
        util.layout.gap = kCellGap;
        st.polarity = &util.emplace<ui::Toggle>(
            "mixer.polarity." + tid, "\u00d8", "polarity",
            [this, tid](bool on) { (void)set(tid, "phase_invert", json::Value(on)); });
        st.mono = &util.emplace<ui::Toggle>(
            "mixer.mono." + tid, "mono", "mono",
            [this, tid](bool on) { (void)set(tid, "mono", json::Value(on)); });
        st.polarity->layout.width = st.mono->layout.width = ui::SizeRule::fraction();
        st.polarity->layout.height = st.mono->layout.height = ui::SizeRule::fixed(kCellH);
        st.polarity->cell = st.mono->cell = true;
        st.pan = &frame->emplace<ui::PanControl>("mixer.pan." + tid);
        // Each bar sets what the strip's pan mode gives it (ADR-0066).
        auto panBar = [this, tid, id, control = st.pan](int part) {
            return [this, tid, id, control, part](double value, ui::Gesture g) {
                if (g == ui::Gesture::Begin) ++gesture_;
                if (g == ui::Gesture::Begin || g == ui::Gesture::End) return;
                const model::Track* now = app_.head().tracks.find(id);
                if (!now) return;
                const char* field = panField(*now, part);
                (void)set(tid, field, json::Value(value),
                          g == ui::Gesture::Move ? "mixer." + std::string(field) + ":" + tid + ":" +
                                                       std::to_string(gesture_)
                                                 : "",
                          bool(control->gestureModifiers() & ui::kAlt));
            };
        };
        st.pan->onChange = panBar(0);
        st.pan->onSecondChange = panBar(1);
        // A drag on a modulated bar edits its route's depth (ADR-0096).
        st.pan->onDepth = [this, id](int part, double depth, ui::Gesture g) {
            const model::Track* now = app_.head().tracks.find(id);
            if (!now) return;
            const char* field = panField(*now, part);
            setDepth({id, field}, depth, g);
        };
        showPan(*st.pan, t);
        // The state row, over the fader: arm, monitor, mute, solo, one cell
        // each and always in that place. A buss has no arm or monitor and a
        // master no monitor: their cells stay empty, so M and S line up
        // across every strip (ADR-0057: a master records its output).
        auto& state = frame->emplace<ui::Node>("mixer.state." + tid);
        state.layout.dir = ui::Layout::Dir::Row;
        state.layout.height = ui::SizeRule::fixed(kCellH);
        state.layout.gap = kCellGap;
        ui::Node* first = nullptr;
        ui::Node* second = nullptr;
        if (t.type != model::StripType::Buss) {
            st.arm = &state.emplace<ui::ArmButton>("mixer.arm." + tid, [this, tid](bool on) {
                (void)set(tid, "armed", json::Value(on));
            });
            first = st.arm;
        } else {
            first = &state.emplace<ui::Node>("mixer.state.1." + tid);
        }
        if (t.type == model::StripType::Channel) {
            st.monitor = &state.emplace<ui::MonitorButton>("mixer.monitor." + tid, [this, tid] {
                const auto parsed = model::Id::parse(tid);
                const model::Track* tr = parsed ? app_.head().tracks.find(*parsed) : nullptr;
                if (!tr) return;
                const char* next = tr->record.monitor == model::Monitor::Off  ? "on"
                                   : tr->record.monitor == model::Monitor::On ? "auto"
                                                                              : "off";
                (void)set(tid, "monitor", json::Value(std::string(next)));
            });
            second = st.monitor;
        } else {
            second = &state.emplace<ui::Node>("mixer.state.2." + tid);
        }
        st.mute = &state.emplace<ui::Toggle>(
            "mixer.mute." + tid, "M", "mute",
            [this, tid](bool on) { (void)set(tid, "mute", json::Value(on)); });
        st.solo =
            &state.emplace<ui::Toggle>("mixer.solo." + tid, "S", "solo", [this, tid](bool on) {
                (void)set(tid, "solo", json::Value(std::string(on ? "on" : "off")));
            });
        st.mute->cell = st.solo->cell = true;
        for (ui::Node* cell :
             {first, second, static_cast<ui::Node*>(st.mute), static_cast<ui::Node*>(st.solo)}) {
            cell->layout.width = ui::SizeRule::fraction();
            cell->layout.height = ui::SizeRule::fixed(kCellH);
        }
        auto& faderRow = frame->emplace<ui::Node>("mixer.faderrow." + tid);
        faderRow.layout.dir = ui::Layout::Dir::Row;
        faderRow.layout.height = ui::SizeRule::fixed(kFaderH);
        // The fader, then the meter with its scale: the scale's ticks end
        // where the meter starts, so its numbers belong to the meter.
        auto& spacer = faderRow.emplace<ui::Node>("mixer.spacer." + tid);
        spacer.layout.width = ui::SizeRule::fraction();
        st.fader = &faderRow.emplace<ui::Fader>("mixer.fader." + tid);
        faderRow.emplace<MeterScale>("mixer.scale." + tid);
        st.meter = &faderRow.emplace<ui::Meter>("mixer.meter." + tid);
        st.meter->layout.width = ui::SizeRule::fixed(16);
        st.meter->setMarks(MeterScale::marks());
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
        st.fader->onDepth = [this, id](double depth, ui::Gesture g) {
            setDepth({id, "volume"}, depth, g);
        };
        // Under the fader its value; under the meter the highest peak shown.
        auto& values = frame->emplace<ui::Node>("mixer.values." + tid);
        values.layout.dir = ui::Layout::Dir::Row;
        values.layout.height = ui::SizeRule::fixed(14);
        st.value = &values.emplace<ui::Label>("mixer.value." + tid, dbText(t.volumeDb),
                                              ui::Font::Mono, "caption", "ink-quiet");
        st.value->align = ui::kCenter | ui::kMiddle;
        st.value->layout.width = ui::SizeRule::fraction();
        st.peak = &values.emplace<PeakReadout>("mixer.peak." + tid, *st.meter);
        st.peak->layout.width = ui::SizeRule::fraction();
        // What the status line calls each control, as on the track's row.
        namespace names = views::names;
        st.fx->hint = names::kChain;
        st.addFx->hint = names::kAddFx;
        st.out->hint =
            t.type == model::StripType::Master ? names::kHardwareOutput : names::kRouting;
        if (st.input) st.input->hint = names::kInput;
        st.polarity->hint = names::kPolarity;
        st.mono->hint = names::kMono;
        st.pan->hint = names::kPan;
        st.mute->hint = names::kMute;
        st.solo->hint = names::kSolo;
        st.fader->hint = names::kVolume;
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
        auto frame = std::make_unique<StripFrame>("mixer.vca." + tid, [this, gid](ui::Point p) {
            // On the fader, mute or solo: the parameter menu.
            if (const char* param = learnableAt(gid.str(), p)) {
                openParamMenu(root_, actions_, gid, param, {p.x, p.y, 1, 1});
                return;
            }
            trackMenu_.openGroup(gid, {}, {p.x, p.y, 1, 1});
        });
        st.node = frame.get();
        frame->emplace<ColorStripe>("mixer.vca.color." + tid, e.value->color);
        frame->emplace<ui::Label>("mixer.vca.name." + tid, e.value->name, ui::Font::UiBold, "small",
                                  "ink");
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
        // M and S in the cells they have on every strip, over the fader.
        auto& ms = frame->emplace<ui::Node>("mixer.vca.ms." + tid);
        ms.layout.dir = ui::Layout::Dir::Row;
        ms.layout.height = ui::SizeRule::fixed(kCellH);
        ms.layout.gap = kCellGap;
        ui::Node* first = &ms.emplace<ui::Node>("mixer.vca.ms.1." + tid);
        ui::Node* second = &ms.emplace<ui::Node>("mixer.vca.ms.2." + tid);
        st.mute = &ms.emplace<ui::Toggle>("mixer.vca.mute." + tid, "M", "mute",
                                          [setVca](bool on) { setVca("mute", on); });
        st.solo = &ms.emplace<ui::Toggle>("mixer.vca.solo." + tid, "S", "solo",
                                          [setVca](bool on) { setVca("solo", on); });
        st.mute->cell = st.solo->cell = true;
        for (ui::Node* cell :
             {first, second, static_cast<ui::Node*>(st.mute), static_cast<ui::Node*>(st.solo)}) {
            cell->layout.width = ui::SizeRule::fraction();
            cell->layout.height = ui::SizeRule::fixed(kCellH);
        }
        st.fader = &frame->emplace<ui::Fader>("mixer.vca.fader." + tid);
        members.hint = {"The group's tracks", {}};
        env.hint = views::names::kEnvelopes;
        st.mute->hint = views::names::kMute;
        st.solo->hint = views::names::kSolo;
        st.fader->hint = views::names::kVolume;
        st.fader->layout.height = ui::SizeRule::fixed(kFaderH);
        st.fader->setValue(e.value->vca->volumeDb);
        st.value =
            &frame->emplace<ui::Label>("mixer.vca.value." + tid, dbText(e.value->vca->volumeDb),
                                       ui::Font::Mono, "caption", "ink-quiet");
        st.fader->onChange = [this, setVca, tid](double db, ui::Gesture g) {
            if (g == ui::Gesture::Begin) ++gesture_;
            if (g == ui::Gesture::Move || g == ui::Gesture::Set)
                setVca("volume_db", db,
                       g == ui::Gesture::Move ? "vca:" + tid + ":" + std::to_string(gesture_) : "");
        };
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

std::optional<TrackTarget> Mixer::trackTargetAt(ui::Point p) const {
    if (!visible() || !panel_->visibleInTree()) return std::nullopt;
    const Strip* strip = stripAt(p);
    const auto id = strip ? model::Id::parse(strip->track) : std::nullopt;
    if (!id || !app_.head().tracks.find(*id)) return std::nullopt;
    const ui::Rect clip =
        strip->node->parent() ? strip->node->parent()->bounds() : strip->node->bounds();
    return TrackTarget{*id, strip->node->bounds().intersect(clip)};
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
    showDragCursor();
    noteDragEdge(p);
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
    dragMods_ = mods;
    if (phase == DragPhase::End) return endDrag(true);
    showDragCursor();
    noteDragEdge(p, phase == DragPhase::Begin);
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
    d.home = over && over->track == track;  // still over the strip it leaves
    const auto from = model::Id::parse(track),
               to = over ? model::Id::parse(over->track) : std::nullopt;
    if (over && over->track != track && over->type != "channel" && from && to &&
        !model::sendsTo(app_.head(), *from, *to)) {
        d.drop = DragView::Drop::Copy;
        d.target = over->track;
        d.marker = over->node->bounds().intersect(over->node->parent()->bounds());
    }
    if (phase == DragPhase::End) return endDrag(true);
    showDragCursor();
    noteDragEdge(p);
    root_.requestAnimationFrame();
}

// A strip dropped between two strips of its own type (a master among masters):
// the index track.move takes, among its parent's children.
void Mixer::resolveStripDrop() {
    DragView& d = *drag_;
    d.drop = DragView::Drop::None;
    d.home = false;
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
    d.home = !index;  // among its own kind, where it already is
    if (!index) return;
    float x = gap < group.size() ? group[gap]->node->bounds().x - 2
                                 : group.back()->node->bounds().right() + 1;
    // Among the strips that scroll, the bar stays where they show: at their
    // edge for a gap that is out of view, not on the masters beside them.
    if (track->type != model::StripType::Master) {
        const ui::Rect area = static_cast<StripRow*>(row_)->scrolledArea();
        x = std::clamp(x, area.x + 1, std::max(area.x + 1, area.right() - 2));
    }
    d.marker = ui::Rect{x - 1, row.y, 3, row.h}.intersect(row);
    d.index = *index;
    d.drop = DragView::Drop::Reorder;
}

// A plug-in dropped among its strip's plug-ins (reorder) or onto another strip
// (a copy, last in that chain).
void Mixer::resolveFxDrop() {
    DragView& d = *drag_;
    d.drop = DragView::Drop::None;
    d.home = false;
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
    d.home = !index;  // in its own chain, where it already is
    if (!index) return;
    // The bar stays where the list shows: at its edge for a gap that is
    // scrolled out of view.
    const ui::Rect list = homeList->bounds();
    const float y = std::clamp(
        gap < slots.size() ? slots[gap]->bounds().y - 2 : slots.back()->bounds().bottom() + 0.5f,
        list.y + 0.5f, std::max(list.y + 0.5f, list.bottom() - 2.5f));
    d.marker = ui::Rect{list.x, y - 0.5f, list.w, 3}.intersect(list);
    d.index = *index;
    d.drop = DragView::Drop::Reorder;
}

// A plug-in's own menu (a right-click on it in the chain).
void Mixer::openFxMenu(model::Id fx, const ui::Rect& slot, ui::Point at) {
    const model::FxInstance* inst = app_.head().fx.find(fx);
    if (!inst) return;
    std::vector<ui::MenuItem> items;
    // First, for a plug-in this run has disabled: loading it again is the likeliest thing wanted.
    if (app_.loadModeOf(inst->plugin) == engine::LoadMode::Disable) {
        ui::MenuItem enable;
        enable.label = "Enable";
        enable.run = [this, fx] {
            json::Value args = json::Value::object();
            args["fx"] = fx.str();
            run("ui.fx.enable", std::move(args));
        };
        items.push_back(std::move(enable));
    }
    ui::MenuItem rename, duplicate, remove;
    rename.label = "Rename…";
    rename.run = [this, fx, slot] { openFxRename(fx, slot); };
    duplicate.label = "Duplicate";
    duplicate.run = [this, fx] { duplicateFx(fx); };
    remove.label = "Delete";
    remove.run = [this, fx] { removeFx(fx); };
    items.push_back(std::move(rename));
    items.push_back(std::move(duplicate));
    items.push_back(std::move(remove));
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

// The pointer while a strip, a plug-in or a send is in hand: the move
// cursor where a release would do something and over the thing's own place,
// "not allowed" over a place that refuses it.
void Mixer::showDragCursor() {
    const bool refused = drag_->drop == DragView::Drop::None && !drag_->home;
    root_.setDragCursor(refused ? ui::Cursor::NotAllowed : ui::Cursor::Move);
}

// Edge scroll (gui.md §4.4): a strip, a plug-in or an output held within
// the zone at the left or right edge of the strips that scroll, or past it,
// scrolls them that way at the arrange view's speeds, so a drop can reach a
// strip that is out of view. The masters, pinned at the right, do not scroll:
// a plug-in or an output over one of them is where it can be dropped, and
// scrolls nothing; a strip there is past the edge of its own kind's strips,
// and scrolls them on.
// A plug-in held over its own strip, within the zone at the top or bottom of
// the strip's plug-in list or past it, scrolls that list, so a drop can
// reach a place in a chain longer than the list shows. The zone the drag
// began in (`begin`: the drag's first point) waits until the pointer has
// left it once: the first and last rows in view lie inside the zones, and
// picking one of them up is not yet a wish to scroll.
void Mixer::noteDragEdge(ui::Point p, bool begin) {
    const auto* row = static_cast<const StripRow*>(row_);
    const ui::Rect area = row->scrolledArea();
    const bool onPinned = drag_->kind != DragView::Kind::Strip && row->pinnedArea().contains(p);
    edgeV_ = onPinned ? 0.0f : ui::edgeScrollSpeed(p.x, area.x, area.right());
    edgeListV_ = 0;
    int zone = 0;  // the list's zone the pointer is inside: top -1, bottom 1
    if (const Strip* over = stripAt(p);
        drag_->kind == DragView::Kind::Fx && over && over->track == drag_->track) {
        const ui::Rect list = over->chain->bounds();
        edgeListV_ = ui::edgeScrollSpeed(p.y, list.y, list.bottom());
        if (p.y >= list.y && p.y <= list.bottom()) zone = (edgeListV_ > 0) - (edgeListV_ < 0);
    }
    if (begin)
        edgeListHeld_ = zone;
    else if (zone != edgeListHeld_)
        edgeListHeld_ = 0;
    if (edgeListHeld_ != 0) edgeListV_ = 0;
    if (!edgeResolving_) edgeIdle_ = false;  // the pointer moved: there may be more to scroll
}

bool Mixer::autoScrolling() const {
    return (edgeV_ != 0 || edgeListV_ != 0) && drag_->kind != DragView::Kind::None;
}

bool Mixer::autoScroll(double seconds) {
    if (!autoScrolling()) {
        edgeV_ = edgeListV_ = 0;
        return false;
    }
    // At an end of the strips nothing more happens until the pointer moves.
    if (edgeIdle_) return false;
    // The drop is resolved again first, against the strips as the last step
    // left them and the layout has since placed them; then the next step.
    const DragView d = *drag_;
    edgeResolving_ = true;
    switch (d.kind) {
        case DragView::Kind::Strip:
            dragStrip(d.track, DragPhase::Move, d.pos);
            break;
        case DragView::Kind::Fx:
            if (const auto fx = model::Id::parse(d.fx))
                dragFx(d.track, *fx, DragPhase::Move, d.pos, dragMods_);
            break;
        case DragView::Kind::Send:
            dragSend(d.track, DragPhase::Move, d.pos);
            break;
        case DragView::Kind::None:
            break;
    }
    edgeResolving_ = false;
    bool moved = static_cast<StripRow*>(row_)->scrollBy(edgeV_ * static_cast<float>(seconds));
    if (edgeListV_ != 0)
        for (const Strip& st : strips_)
            if (auto* list = dynamic_cast<FxList*>(st.chain); list && st.track == drag_->track)
                moved = list->scrollBy(edgeListV_ * static_cast<float>(seconds)) || moved;
    edgeIdle_ = !moved && seconds > 0;
    return moved;
}

// The drag is over: drop it (one command, one undo step) or let it go.
void Mixer::endDrag(bool commit) {
    const DragView done = *drag_;
    edgeV_ = edgeListV_ = 0;
    edgeListHeld_ = 0;
    edgeIdle_ = false;
    *drag_ = DragView{};
    root_.setDragCursor(std::nullopt);
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

std::optional<views::ControlModulation> Mixer::modulationOf(const model::Session& s,
                                                            const model::EnvTarget& target) {
    const auto catalog = racks::catalog(app_);
    return views::controlModulation(
        s, target,
        [&catalog](const model::Session& at, const model::ModSource& source) {
            return catalog && racks::unipolarSource(at, *catalog, source);
        },
        &live_);
}

void Mixer::setDepth(const model::EnvTarget& target, double depth, ui::Gesture g) {
    if (g == ui::Gesture::Begin) ++gesture_;
    if (g == ui::Gesture::Begin || g == ui::Gesture::End) return;
    const auto mod = modulationOf(app_.head(), target);
    if (!mod) return;
    json::Value args = json::Value::object();
    args["modulation"] = mod->route.str();
    args["depth"] = depth;
    cmd::Origin origin = kOrigin;
    if (g == ui::Gesture::Move)
        origin.gesture = "mixer.depth:" + mod->route.str() + ":" + std::to_string(gesture_);
    if (auto r = dispatcher_.execute({"modulation.set", std::move(args), origin});
        !r && actions_.onError)
        actions_.onError(std::string("mixer: ") + r.error().message);
}

// The fader's and each pan bar's modulation: a bar beside it, and a drag on
// the bar for depth (ui::ModulationView). A dual pan's bars take none.
void Mixer::showModulation(Strip& st, const model::Track& t) {
    const model::Session& s = app_.head();
    const model::Id id = *model::Id::parse(st.track);
    auto view = [&](const char* param) -> std::optional<ui::ModulationView> {
        if (!views::stripModulatable(s, id, param)) return std::nullopt;
        const auto mod = modulationOf(s, {id, param});
        return mod ? std::optional(mod->view) : std::nullopt;
    };
    st.fader->setModulation(view("volume"));
    const bool dual = t.type != model::StripType::Master && t.panMode == model::PanMode::Dual;
    st.pan->setModulation(0, dual ? std::nullopt : view("pan"));
    st.pan->setModulation(
        1, !dual && t.type != model::StripType::Master ? view("width") : std::nullopt);
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
        showPan(*st.pan, *t);
        showModulation(st, *t);
        st.mute->setOn(t->mute);
        st.solo->setOn(t->solo != model::Solo::Off);
        st.polarity->enabled = st.mono->enabled = !t->freeze;
        st.addFx->enabled = !t->freeze;
        st.fx->setEmphasis(!t->chain.empty() || !t->inputChain.empty());
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

void Mixer::sync(std::chrono::steady_clock::time_point now) {
    if (!visible()) return;
    // A drag held at the strips' edge scrolls by the time since the last pass
    // (capped, so a stalled frame is not a jump).
    if (autoScrolling()) {
        const double seconds = edgeClock_.step(now);
        if (autoScroll(seconds) || seconds == 0) root_.requestAnimationFrame();
    } else {
        edgeClock_.reset();
    }
    follow();
    // Modulation telemetry (ADR-0096, M2): the modulated controls' live
    // values, redrawn only when they move.
    if (auto live = app_.modulationOffsets(); live != live_) {
        live_ = std::move(live);
        const model::Session& s = app_.head();
        for (Strip& st : strips_)
            if (const auto id = model::Id::parse(st.track))
                if (const model::Track* t = s.tracks.find(*id)) showModulation(st, *t);
    }
}

// The pass without its time: what the panel shows follows the window, the
// selection and the session.
void Mixer::follow() {
    if (!visible()) return;
    applyHeight();  // the window may have been resized
    if (actions_.ui().selectedTracks != shownSelection_) showSelection();  // the TCP changed it
    const bool replaced = app_.projectGeneration() != projectGeneration_;
    // A load override or a failed plug-in changes the strips without a new project version.
    const bool attention = attention_.poll();
    if (!replaced && !attention && app_.head().version == version_) return;
    projectGeneration_ = app_.projectGeneration();
    version_ = app_.head().version;
    const std::string structureNow = structure();
    if (replaced || structureNow != structure_) {
        structure_ = structureNow;
        rebuild(!replaced);
    }
    update();
}

}  // namespace supple::gui
