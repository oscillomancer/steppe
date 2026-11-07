// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "mouse_dialog.h"
#include "supple/ui/shortcut.h"

#include "supple/ui/forms.h"
#include "supple/ui/popup.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

std::string comboLabel(unsigned mods) {
    std::string name = views::MouseMap::comboName(mods);
    if (name.empty()) return "plain";
    for (std::size_t i = 0; i < name.size(); ++i)
        if (i == 0 || name[i - 1] == '+')
            name[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[i])));
    return ui::shortcutLabel(std::move(name));
}

std::string behaviourLabel(std::string_view b) {
    if (b == "resize_all") return "resize all track heights";
    if (b == "scroll") return "scroll the tracks";
    if (b == "scroll_time") return "scroll in time";
    if (b == "scroll_pitch") return "scroll the pitches";
    if (b == "zoom") return "zoom in time";
    if (b == "zoom_rows") return "zoom the keys and rows";
    std::string s(b);
    for (char& c : s)
        if (c == '_') c = ' ';
    if (const auto free = s.find(" free"); free != std::string::npos)
        s = s.substr(0, free) + " (off the grid)";
    return s;
}

}  // namespace

void MouseDialog::open(std::string context) {
    if (!views::MouseMap::context(context)) context = "item.drag";
    context_ = context;
    if (root_.popupOpen()) root_.closePopups();
    buttons_.clear();
    auto form = std::make_unique<ui::Node>("mouse");
    form->layout.dir = ui::Layout::Dir::Column;
    form->layout.width = ui::SizeRule::fixed(420);
    const auto combos = views::MouseMap::combos();
    form->layout.height =
        ui::SizeRule::fixed(96.0f + 30.0f * static_cast<float>(combos.size()) + 36.0f);
    form->layout.padding = ui::Insets::all(12);
    form->layout.gap = 4;
    form->emplace<ui::Label>("mouse.title", "Mouse modifiers", ui::Font::UiBold, "body");
    // The context: a dropdown that lists them all (it draws its chevron; the
    // UI font has no arrow glyphs).
    std::vector<ui::Choice> contexts;
    for (const views::MouseMap::Context& c : views::MouseMap::contexts())
        contexts.push_back({std::string(c.id), std::string(c.label)});
    auto& pick = form->emplace<ui::Dropdown>("mouse.context", std::move(contexts), context_);
    pick.layout.height = ui::SizeRule::fixed(26);
    pick.onChange = [this](const std::string& id) { open(std::string(id)); };
    for (unsigned combo : combos) {
        auto& row = form->emplace<ui::Node>("mouse.row." + views::MouseMap::comboName(combo));
        row.layout.dir = ui::Layout::Dir::Row;
        row.layout.height = ui::SizeRule::fixed(26);
        row.layout.gap = 8;
        auto& label = row.emplace<ui::Label>("mouse.combo", comboLabel(combo), ui::Font::Mono,
                                             "caption", "ink-quiet");
        label.layout.width = ui::SizeRule::fixed(120);
        auto& b = row.emplace<ui::Button>("mouse.set." + views::MouseMap::comboName(combo), "");
        buttons_.emplace_back(combo, &b);
        b.layout.width = ui::SizeRule::fraction();
        b.onClick = [this, combo, at = &b] {
            std::vector<ui::MenuItem> items;
            const views::MouseMap::Context& c = *views::MouseMap::context(context_);
            for (std::string_view behaviour : c.behaviours) {
                ui::MenuItem m;
                m.label = behaviourLabel(behaviour);
                m.checked =
                    map_.bound(context_, combo) && map_.behaviour(context_, combo) == behaviour;
                m.run = [this, combo, value = std::string(behaviour)] {
                    set(context_, combo, value);
                };
                items.push_back(std::move(m));
            }
            if (combo != 0) {
                ui::MenuItem plain;
                plain.label = "Same as the plain press";
                plain.checked = !map_.bound(context_, combo);
                plain.run = [this, combo] { set(context_, combo, ""); };
                items.push_back(std::move(plain));
            }
            root_.openPopup(
                ui::makeMenu("mouse.behaviours", std::move(items), [this] { root_.closePopup(); }),
                at->bounds());
        };
    }
    auto& reset = form->emplace<ui::Button>("mouse.reset", "Reset this context",
                                            [this] { resetContext(context_); });
    reset.layout.height = ui::SizeRule::fixed(26);
    root_.openPopup(std::move(form), {}, ui::Placement::Center);
    refresh();
}

void MouseDialog::refresh() {
    if (!root_.find("mouse")) {  // closed: its buttons are gone
        buttons_.clear();
        return;
    }
    for (auto [combo, button] : buttons_)
        button->setText(behaviourLabel(map_.behaviour(context_, combo)) +
                        (combo != 0 && !map_.bound(context_, combo) ? "  (plain)" : ""));
}

void MouseDialog::set(const std::string& context, unsigned mods, const std::string& behaviour) {
    if (auto r = map_.set(context, mods, behaviour); !r) {
        if (onError) onError(r.error().toString());
        return;
    }
    save();
    if (context == context_) refresh();
}

void MouseDialog::resetContext(const std::string& context) {
    const views::MouseMap defaults;
    for (unsigned combo : views::MouseMap::combos())
        (void)map_.set(context, combo,
                       defaults.bound(context, combo)
                           ? std::string(defaults.behaviour(context, combo))
                           : std::string());
    save();
    if (context == context_) refresh();
}

void MouseDialog::save() {
    if (auto r = map_.save(file_); !r && onError) onError("mouse: " + r.error().toString());
}

}  // namespace supple::gui
