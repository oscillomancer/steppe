// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/ui/forms.h"

#include <SDL3/SDL_keycode.h>

#include <algorithm>

#include "supple/ui/popup.h"
#include "supple/ui/root.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"

namespace supple::ui {
namespace {
// The chevron on the right of a dropdown face.
void chevron(Canvas& canvas, const Rect& b, Color ink) {
    const float x = b.right() - 12, y = b.y + b.h / 2;
    canvas.fillPath({{x - 4, y - 2}, {x + 4, y - 2}, {x, y + 3}}, ink);
}
// A button that draws a down chevron (the UI font has no arrow glyphs).
class ChevronButton : public Button {
  public:
    ChevronButton(std::string id, std::function<void()> click)
        : Button(std::move(id), "", std::move(click)) {}
    void paint(Canvas& canvas) override {
        Button::paint(canvas);
        const Theme& t = canvas.theme();
        const Rect b = bounds();
        chevron(canvas, {b.x, b.y, b.w / 2 + 12, b.h},
                enabled ? t.color("ink-quiet") : t.color("ink-faint"));
    }
};

// A fold section's header: a triangle (right when folded, down when open)
// and the title.
class FoldHeader : public Button {
  public:
    FoldHeader(std::string id, std::string title, std::function<void()> click)
        : Button(std::move(id), std::move(title), std::move(click)) {}
    bool open = false;
    Size measure(const Canvas& canvas, Size available) override {
        Size size = Button::measure(canvas, available);
        size.w += 14;
        return size;
    }
    void paint(Canvas& canvas) override {
        const Theme& t = canvas.theme();
        const Rect b = bounds();
        if (hovered()) canvas.fillRound(b, t.radius("s"), t.color("raised"));
        if (focused()) canvas.strokeRound(b, t.radius("s"), t.color("focus"));
        const float x = b.x + 8, y = b.y + b.h / 2;
        const Color ink = t.color("ink-quiet");
        if (open)
            canvas.fillPath({{x - 4, y - 2}, {x + 4, y - 2}, {x, y + 3}}, ink);
        else
            canvas.fillPath({{x - 2, y - 4}, {x + 3, y}, {x - 2, y + 4}}, ink);
        canvas.text({b.x + 18, b.y, std::max(0.0f, b.w - 18), b.h}, valueText(), Font::UiBold,
                    t.type("small"), t.color("ink"), kLeft | kMiddle);
    }
};
}  // namespace

// ------------------------------------------------------------------ Dropdown

Dropdown::Dropdown(std::string id, std::vector<Choice> choices, std::string value)
    : Node(std::move(id), Role::Button), choices_(std::move(choices)), value_(std::move(value)) {
    focusable = true;
    interactive = true;
    layout.width = SizeRule::content();
    layout.height = SizeRule::fixed(26);
}

void Dropdown::setChoices(std::vector<Choice> choices) {
    choices_ = std::move(choices);
    invalidate();
    if (layout.width.kind == SizeRule::Kind::Content) relayout();
}

void Dropdown::setValue(const std::string& value) {
    if (value == value_) return;
    value_ = value;
    invalidate();
}

std::string Dropdown::label() const {
    for (const auto& c : choices_)
        if (c.value == value_) return c.label;
    return {};
}

void Dropdown::choose(const std::string& value) {
    setValue(value);
    if (auto callback = onChange) callback(value);
}

Size Dropdown::measure(const Canvas& canvas, Size) {
    const Theme& t = canvas.theme();
    float widest = 0;
    for (const auto& c : choices_)
        widest = std::max(widest, canvas.textWidth(c.label, Font::Ui, t.type("small")));
    return {widest + 2 * t.space("m") + 16, 26};
}

void Dropdown::paint(Canvas& canvas) {
    const Theme& t = canvas.theme();
    const Rect b = bounds();
    const Color face = !enabled    ? t.color("panel")
                       : pressed_  ? t.color("selection")
                       : hovered() ? t.color("raised")
                                   : t.color("panel");
    canvas.fillRound(b, t.radius("s"), face);
    canvas.strokeRound(b, t.radius("s"), focused() ? t.color("focus") : t.color("rule"));
    const Rect inner{b.x + t.space("s"), b.y, std::max(0.0f, b.w - t.space("s") - 20), b.h};
    const Color ink = !enabled ? t.color("ink-faint")
                      : quiet  ? t.color("ink-quiet")
                               : t.color("ink");
    canvas.save();
    canvas.clip(inner);
    canvas.text(inner, label(), Font::Ui, t.type("small"), ink, kLeft | kMiddle);
    canvas.restore();
    chevron(canvas, b, enabled ? t.color("ink-quiet") : t.color("ink-faint"));
}

void Dropdown::open() {
    Root* r = root();
    if (!r || !enabled) return;
    std::vector<MenuItem> items;
    for (const auto& c : choices_) {
        MenuItem item;
        item.label = c.label;
        item.enabled = c.enabled;
        item.checked = c.value == value_;
        item.run = [this, value = c.value] { choose(value); };
        items.push_back(std::move(item));
    }
    r->openPopup(makeMenu(id() + ".menu", std::move(items), [r] { r->closePopup(); }), bounds());
}

bool Dropdown::onPointer(const PointerEvent& e) {
    if (e.button != 1 && e.type != PointerEvent::Type::Cancel) return false;
    switch (e.type) {
        case PointerEvent::Type::Down:
            pressed_ = true;
            invalidate();
            return true;
        case PointerEvent::Type::Cancel:
            pressed_ = false;
            invalidate();
            return true;
        case PointerEvent::Type::Up: {
            const bool fire = pressed_ && bounds().contains(e.pos);
            pressed_ = false;
            invalidate();
            if (fire) open();
            return true;
        }
        default:
            return false;
    }
}

bool Dropdown::onKey(const KeyEvent& e) {
    if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER || e.key == SDLK_DOWN) {
        open();
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ EditableDropdown

EditableDropdown::EditableDropdown(std::string id, std::vector<Choice> suggestions,
                                   std::string text, std::string placeholder)
    : Node(id), suggestions_(std::move(suggestions)) {
    layout.dir = Layout::Dir::Row;
    layout.gap = 2;
    layout.height = SizeRule::fixed(26);
    layout.align = Layout::Align::Stretch;
    field_ = &emplace<TextField>(id + ".text", std::move(text), std::move(placeholder));
    field_->layout.width = SizeRule::fraction();
    field_->onChange = [this](const std::string& t) {
        if (auto callback = onChange) callback(t);
    };
    button_ = &emplace<ChevronButton>(id + ".pick", [this] {
        Root* r = root();
        if (!r || suggestions_.empty()) return;
        std::vector<MenuItem> items;
        for (const auto& s : suggestions_) {
            MenuItem item;
            item.label = s.label;
            item.enabled = s.enabled;
            item.checked = s.value == field_->text();
            item.run = [this, value = s.value] { pick(value); };
            items.push_back(std::move(item));
        }
        r->openPopup(makeMenu(this->id() + ".menu", std::move(items), [r] { r->closePopup(); }),
                     bounds());
    });
    button_->layout.width = SizeRule::fixed(24);
    button_->layout.height = SizeRule::fixed(26);
}

void EditableDropdown::setSuggestions(std::vector<Choice> suggestions) {
    suggestions_ = std::move(suggestions);
}
void EditableDropdown::setText(const std::string& text) {
    if (text != field_->text()) field_->setText(text);
}
void EditableDropdown::setPlaceholder(std::string placeholder) {
    field_->setPlaceholder(std::move(placeholder));
}
const std::string& EditableDropdown::text() const { return field_->text(); }
void EditableDropdown::pick(const std::string& value) {
    field_->setText(value);
    if (auto callback = onChange) callback(value);
}
std::string EditableDropdown::valueText() const { return field_->text(); }

// ------------------------------------------------------------------ Checkbox

Checkbox::Checkbox(std::string id, std::string text, bool checked)
    : Node(std::move(id), Role::Toggle), text_(std::move(text)), checked_(checked) {
    focusable = true;
    interactive = true;
    layout.width = SizeRule::content();
    layout.height = SizeRule::fixed(24);
}

void Checkbox::setChecked(bool checked) {
    if (checked == checked_) return;
    checked_ = checked;
    invalidate();
}

void Checkbox::setText(std::string text) {
    if (text == text_) return;
    text_ = std::move(text);
    invalidate();
    if (layout.width.kind == SizeRule::Kind::Content) relayout();
}

void Checkbox::toggle() {
    if (!enabled) return;
    checked_ = !checked_;
    invalidate();
    if (auto callback = onToggle) callback(checked_);
}

Size Checkbox::measure(const Canvas& canvas, Size) {
    const Theme& t = canvas.theme();
    return {22 + canvas.textWidth(text_, Font::Ui, t.type("small")) + t.space("s"), 24};
}

void Checkbox::paint(Canvas& canvas) {
    const Theme& t = canvas.theme();
    const Rect b = bounds();
    const Rect box{b.x + 2, b.y + (b.h - 14) / 2, 14, 14};
    canvas.fillRound(box, 2, checked_ && enabled ? t.color("accent") : t.color("panel"));
    canvas.strokeRound(box, 2, focused() ? t.color("focus") : t.color("rule"));
    if (checked_) {
        const Color mark = enabled ? t.color("ground") : t.color("ink-faint");
        canvas.line({box.x + 3, box.y + 7}, {box.x + 6, box.y + 10.5f}, mark, 2);
        canvas.line({box.x + 6, box.y + 10.5f}, {box.x + 11.5f, box.y + 3.5f}, mark, 2);
    }
    const Rect label{b.x + 22, b.y, std::max(0.0f, b.w - 22), b.h};
    canvas.text(label, text_, Font::Ui, t.type("small"),
                enabled ? t.color("ink") : t.color("ink-faint"), kLeft | kMiddle);
}

bool Checkbox::onPointer(const PointerEvent& e) {
    if (e.button != 1 && e.type != PointerEvent::Type::Cancel) return false;
    switch (e.type) {
        case PointerEvent::Type::Down:
            pressed_ = true;
            return true;
        case PointerEvent::Type::Cancel:
            pressed_ = false;
            return true;
        case PointerEvent::Type::Up: {
            const bool fire = pressed_ && bounds().contains(e.pos);
            pressed_ = false;
            if (fire) toggle();
            return true;
        }
        default:
            return false;
    }
}

bool Checkbox::onKey(const KeyEvent& e) {
    if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) {
        toggle();
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ FoldSection

FoldSection::FoldSection(std::string id, std::string title, bool open)
    : Node(id), title_(std::move(title)), open_(open) {
    layout.dir = Layout::Dir::Column;
    layout.height = SizeRule::content();
    layout.gap = 4;
    auto& head = emplace<Node>(id + ".head");
    head.layout.dir = Layout::Dir::Row;
    head.layout.height = SizeRule::fixed(26);
    head.layout.gap = 8;
    head.layout.align = Layout::Align::Center;
    header_ = &head.emplace<FoldHeader>(id + ".toggle", title_, [this] {
        setOpen(!open_);
        if (auto callback = onToggle) callback(open_);
    });
    summary_ = &head.emplace<Label>(id + ".summary", "", Font::Ui, "caption", "ink-quiet");
    summary_->layout.width = SizeRule::fraction();
    body_ = &emplace<Node>(id + ".body");
    body_->layout.dir = Layout::Dir::Column;
    body_->layout.height = SizeRule::content();
    body_->layout.gap = 6;
    body_->layout.padding = Insets{16, 0, 0, 0};
    setOpen(open);
}

void FoldSection::setOpen(bool open) {
    open_ = open;
    static_cast<FoldHeader*>(header_)->open = open_;
    header_->invalidate();
    body_->visible = open_;
    summary_->visible = !open_;
    relayout();
}

void FoldSection::setSummary(std::string summary) { summary_->setText(std::move(summary)); }

// ------------------------------------------------------------------ Checklist

Checklist::Checklist(std::string id) : Node(id, Role::List) {
    layout.dir = Layout::Dir::Column;
    layout.height = SizeRule::content();
    layout.gap = 2;
    auto& head = emplace<Node>(id + ".head");
    head.layout.dir = Layout::Dir::Row;
    head.layout.height = SizeRule::fixed(26);
    head.layout.gap = 6;
    head.emplace<Button>(id + ".all", "All", [this] { setAll(true); });
    head.emplace<Button>(id + ".none", "None", [this] { setAll(false); });
    rows_ = &emplace<Node>(id + ".rows");
    rows_->layout.dir = Layout::Dir::Column;
    rows_->layout.height = SizeRule::content();
}

void Checklist::setItems(const std::vector<Choice>& items, const std::vector<std::string>& on) {
    rows_->clear();
    boxes_.clear();
    for (std::size_t i = 0; i < items.size(); ++i) {
        const bool checked = std::find(on.begin(), on.end(), items[i].value) != on.end();
        auto& box =
            rows_->emplace<Checkbox>(id() + ".item." + std::to_string(i), items[i].label, checked);
        box.onToggle = [this](bool) { changed(); };
        boxes_.emplace_back(items[i].value, &box);
    }
    relayout();
}

std::vector<std::string> Checklist::checked() const {
    std::vector<std::string> out;
    for (const auto& [value, box] : boxes_)
        if (box->checked()) out.push_back(value);
    return out;
}

void Checklist::setChecked(const std::vector<std::string>& on) {
    for (auto& [value, box] : boxes_)
        box->setChecked(std::find(on.begin(), on.end(), value) != on.end());
}

void Checklist::setAll(bool on) {
    for (auto& [value, box] : boxes_) box->setChecked(on);
    changed();
}

void Checklist::changed() {
    if (auto callback = onChange) callback(checked());
}

std::string Checklist::valueText() const {
    std::string out;
    for (const auto& value : checked()) out += (out.empty() ? "" : ",") + value;
    return out;
}

}  // namespace supple::ui
